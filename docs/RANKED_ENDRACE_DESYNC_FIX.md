# Ranked End-Race Desync & Position Reporting Architecture

## 1. Executive Summary & Root Cause

Players in Ranked races frequently encounter an issue where they cross the finish line in 1st place (or other winning positions) on their local client screen, but receive negative MMR (e.g. -45 MMR) and are scored by the backend server as **DNF** or last place.

### The Core Architectural Flaw
The ranked result pipeline relies entirely on a **Host-Authoritative Single-Reporter Model**:
1. Every player client sends a race report to `POST /ryunet/ranked-result`.
2. However, the server (`server/ranked_results.go:305`) explicitly filters reports:
   ```go
   if reporter != session.host {
       writeJSON(w, http.StatusOK, map[string]any{"status": "pending"})
       return
   }
   rankedResolve(w, race, now, order, track, reporter)
   ```
3. Only the report from `session.host` is evaluated. All guest submissions are kept solely as dormant votes and are never used to compute standings or deltas.
4. If a guest finishes 1st on their machine, but their finish packet is delayed, dropped, or reordered across the P2P mesh before reaching the host, the host's game client assumes the guest did not complete the race in time (marking them DNF or placing them at the back).
5. The host submits this distorted order to the server. The server scores this as truth, immediately penalizing the winning player with the DNF/last-place MMR formula:
   $$\Delta_{\text{DNF}} = \text{rankedBasePoints}[7] - \text{rankedDNFPenalty}[\text{mode}] = -35 - 10 = -45 \text{ MMR}$$

---

## 2. In-Depth Trace: Client Execution Flow

```mermaid
sequenceDiagram
    autonumber
    participant Engine as Native Game Engine
    participant Capture as capture_finish_order()
    participant RankedEnd as runtime_ranked_race_end()
    participant Outbox as ranked_sender (Worker Thread)
    participant Server as RyuNET Server

    Engine->>Capture: race_results_received(event)
    Note over Capture: Reads event + 0x20 + rank*4 (P2P finish IDs)<br/>Attempts bubble sort on team_finish_time
    Capture->>Capture: finish_order = order; finish_order_count = count
    Engine->>RankedEnd: runtime_ranked_race_end()
    RankedEnd->>RankedEnd: runtime_online_mode_ranked_order(order, 8)
    Note over RankedEnd: Re-reads live racer_manager objects<br/>Maps finish_order to names & flags
    RankedEnd->>Outbox: Spawns pthread with struct ranked_outbox
    Outbox->>Server: POST /ryunet/ranked-result?name=<nick>
```

### Critical Client Vulnerabilities

#### A. Incomplete Finish-Time Sorting in `capture_finish_order()`
Located in `src/runtime_online_mode.c:1653-1665`:
```c
for (int i = 1; i < count; ++i)
    for (int j = i; j > 0 && times[j - 1] > 0 && times[j] > 0 && times[j] < times[j - 1]; --j) {
        // Swaps order entries if both have non-zero times
    }
```
* **Failure mode**: If a guest's packet is delayed past the timeout, the host has `times[j] == 0`. The condition `times[j] > 0` fails, so the guest is never reordered to the front, remaining in the DNF slot.

#### B. Reading Live Mutable State in `runtime_online_mode_ranked_order()`
Located in `src/runtime_online_mode.c:1673-1702`:
* When `capture_finish_order()` runs, it saves `finish_order[]` containing racer keys.
* However, `runtime_online_mode_ranked_order()` is executed later and loops over live `racer_manager` memory pointers:
  ```c
  unsigned char *player = handle_object((void **)(racer + 0x10));
  const char *name = pointer_at(player, 0x68);
  out[written].finished = team_result_count
      ? !!(team_finished_mask & (1u << rank))
      : team_racer_finished(racer);
  ```
* Native engine callbacks during post-race state transitions can recycle or clear `racer` objects, causing corrupted or empty names and incorrect `finished` booleans.

#### C. AI Slot Offset Discrepancy
If AI racers are present, their slots exist in the visual finish order, but they lack human online IDs. When converting to `pos`, indexing gaps can desynchronize position numbering between client expectation and server validation.

---

## 3. Client Mitigation Plan

### Phase 1: Pre-Emptive State Snapshot (P0)
Move data extraction directly into `capture_finish_order()` while native memory is strictly valid and coherent:

```c
struct frozen_ranked_entry {
    char name[32];
    uint64_t online_id;
    int network_id;
    int finished;
    int time_ms;
    int pos;
};
static struct frozen_ranked_entry frozen_ranked_snapshot[8];
static int frozen_ranked_count;
```

1. Inside `capture_finish_order()`, capture name, completion state, and finish times immediately.
2. In `runtime_online_mode_ranked_order()`, copy directly from `frozen_ranked_snapshot` rather than dereferencing live engine memory.

### Phase 2: Missing Finish Packet Lap Recovery (P1)
In `src/runtime_online_mode.c`, inspect racer lap counters when `times[position] == 0`:
* If network timestamps prove all required laps (e.g. 3 or 5 laps) were completed, reconstruct `time_ms` by summing the lap times.
* Set `finished = 1` and let the sorting logic position the racer accurately by time.

### Phase 3: Diagnostic Payload Logging (P0)
Add explicit stderr logging when generating the outgoing payload in `runtime_ranked.c` (`ranked_sender`):
```c
fprintf(stderr, "[Ranked TX] Room %llu Track %s Mode %d Count %d\n",
        (unsigned long long)out->room, out->track, out->mode, out->count);
for (int i = 0; i < out->count; ++i) {
    fprintf(stderr, "  Pos %d: %s | Finish: %d | Time: %d ms\n",
            out->order[i].pos, out->order[i].name,
            out->order[i].finished, frozen_ranked_snapshot[i].time_ms);
}
```

---

## 4. Verification & Testing Protocol

1. **Simulated High-Latency Host**:
   - Throttle UDP port to 250ms+ latency on guest client.
   - Verify that guest finishing ahead of host is captured with valid lap timestamps and not relegated to DNF.
2. **Payload Inspection**:
   - Verify stderr log shows exact parity between HUD placement and JSON output sent to RyuNET.
