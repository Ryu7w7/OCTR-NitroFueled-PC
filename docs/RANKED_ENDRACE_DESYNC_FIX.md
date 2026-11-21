# Ranked End-Race Position Desync — Client Fix Plan

## Problem

Players finish in winning positions on their screen but server scores them as DNF or last place, causing incorrect negative MMR adjustments.

**Root cause**: the server currently resolves race outcomes exclusively from the **room host's report**. If host-side P2P finish packets arrive out-of-order or late due to network latency, the host records players incorrectly.

## Current Client Flow

```
race_results_received()
  -> capture_finish_order(event)         // captures finish order
  -> runtime_ranked_race_end()
    -> runtime_online_mode_ranked_order() // reads racer_manager (live state)
    -> ranked_sender thread
      -> POST /ryunet/ranked-result      // signed payload with full finish order
```

### Known Desync Points (Client Side)

#### 1. `capture_finish_order()` — Late P2P Finish Events

File: `src/runtime_online_mode.c` (`capture_finish_order`)

The host determines positions from incoming network race finish events (`event + 0x20 + rank * 4`). If a player's finish-line packet arrives late across the network, the host flags that player as not finished or places them at the back.

The current lap-time sorting mitigation only adjusts order when *both* racers have valid recorded finish times. If a player's finish event did not register on the host, they remain marked as DNF.

#### 2. `runtime_online_mode_ranked_order()` — Reading Live State

File: `src/runtime_online_mode.c` (`runtime_online_mode_ranked_order`)

The function reads live `racer_manager` memory structures when building the report. Subsequent native UI callbacks or memory updates can alter racer objects before the sender thread finishes generating the payload.

#### 3. AI Slot Inconsistencies

AI slots occupy indices in internal arrays, which can introduce discrepancies between raw visual rank and sanitized human-only positions sent upstream.

---

## Technical Solutions

### Phase 1: Freeze Racer State at Event Capture (P0)

Take an immediate static snapshot of racer identities, names, finish flags, and finish times inside `capture_finish_order()` as soon as the finish event triggers.

```c
static struct {
    char name[32];
    int finished;
    int time_ms;
    int pos;
} frozen_ranked_racers[8];
static int frozen_ranked_count;
```

`runtime_online_mode_ranked_order()` will read directly from this static snapshot rather than dereferencing live game pointers.

### Phase 2: Complete-Lap Validation for Missing Finish Packets (P1)

If all required laps for a racer are present in network lap timestamps but their finish flag was dropped, mark them as finished and calculate total elapsed time from lap sums.

### Phase 3: Per-Player Self-Report Architecture (v2)

Transition from host-only reporting to individual participant reporting:
- Each client sends its own completion status, total time, and lap timestamps.
- Eliminates host network jitter affecting other participants.

---

## Implementation Matrix

| Phase | Description | Impact |
|---|---|---|
| **P0** | Snapshot racer data immediately on race finish event | Eliminates race condition mutations |
| **P0** | Diagnostic output on ranked payload generation | Verifiable reporting logs |
| **P1** | Recover missing finish flags via lap timestamp validation | Prevents false DNF on dropped finish packet |
| **P2** | Individual self-report endpoint (`/ryunet/ranked-result-v2`) | Full decentralization of result authority |
