// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdlib>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <setjmp.h>
#include "../src/runtime_friends.h"
#include "../src/runtime_net_ping.h"
#include "../src/runtime_chat.h"
#include "../src/runtime_spectate.h"
#include "../src/runtime_replay_list.h"
#include "../src/runtime_name_colors.h"
// Renderer tests have no guest process. Clock/host-thread services work; guest accesses abort.
extern "C" {
thread_local sigjmp_buf* runtime_fault_recover = nullptr;
uint32_t runtime_disabled_optimizations = 0;
void (*runtime_thread_started_hook)(const char*) = nullptr;
int runtime_memory_is_mapped(uintptr_t, uint64_t) { return 0; }
int runtime_graphics_tab_ready = 0;
unsigned runtime_friends_serial() { return 0; }
void runtime_chat_start() {}
void runtime_chat_set_enabled(int) {}
void runtime_chat_set_public(int) {}
void runtime_chat_set_in_race(int) {}
int runtime_chat_available() { return 0; }
int runtime_chat_racing() { return 0; }
unsigned runtime_chat_serial() { return 0; }
unsigned runtime_chat_view(RcLine*, int, int* count, int* status) { *count = *status = 0; return 0; }
int runtime_chat_send(const char*) { std::abort(); }
void runtime_chat_mute(const char*, int) { std::abort(); }
int runtime_chat_muted(const char*) { return 0; }
void runtime_spectate_set_share(int) {}
void runtime_spectate_watch(const char*) { std::abort(); }
void runtime_spectate_stop() { std::abort(); }
int runtime_spectate_following(char*, int) { return 0; }
unsigned runtime_spectate_status(int* state, char*, int) { *state = RS_IDLE; return 0; }
int runtime_spectate_viewers() { return 0; }
int runtime_replay_list(RrListEntry*, int) { return 0; }
int runtime_replay_play_file(const char*) { std::abort(); }
int runtime_replay_can_start() { return 0; }
const char* runtime_replay_blocked_reason() { return "No guest in renderer tests"; }
int runtime_replay_playing() { return 0; }
void runtime_replay_folder(char*, size_t) { std::abort(); }
int runtime_replay_export(const char*, char*, size_t) { std::abort(); }
int runtime_replay_import() { std::abort(); }
int runtime_replay_delete(const char*) { std::abort(); }
int runtime_replay_view(RrView*) { return 0; }
void runtime_name_colors_start() {}
int runtime_name_color(const char*, float[3]) { return 0; }
void runtime_friends_view(RfView* out) { *out = {}; }
void runtime_friends_action(const char*, const char*) { std::abort(); }
void runtime_friends_search(const char*) { std::abort(); }
void runtime_friends_report(const char*, const char*, const char*) { std::abort(); }
void runtime_friends_set_hidden(int) { std::abort(); }
void runtime_friends_join(const char*) { std::abort(); }
int runtime_friends_take_event(char*) { return 0; }
int runtime_friends_take_message(char*) { return 0; }
int runtime_net_ping_view(PingPeer*, int) { return 0; }
uint64_t runtime_experiment_bits = 0;
uint64_t runtime_tsc_frequency() { return 1000000000; }
int runtime_file_translate(const char*, char*, size_t) { std::abort(); }
uint64_t runtime_memory_clamp(uintptr_t, uint64_t) { std::abort(); }
uint64_t runtime_process_time_us() { return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
int runtime_memory_region(uintptr_t, uintptr_t*, uintptr_t*, int*) { std::abort(); }
void runtime_memory_set_gpu_hooks(void (*)(uintptr_t, uint64_t),
    void (*)(uintptr_t, uint64_t), void (*)(uintptr_t, uint64_t)) { std::abort(); }
void runtime_thread_attach_host(const char*) {}
void runtime_memory_gpu_protect(uintptr_t, uint64_t, int, int) { std::abort(); }
void runtime_restart() { std::abort(); }
int runtime_cinematic_mode() { return 0; }
uint64_t runtime_process_time_counter() { return runtime_process_time_us() * 1000; }
int32_t* runtime_errno() { std::abort(); }
int runtime_memory_write_backing(uintptr_t, const void*, uint64_t) { std::abort(); }
void runtime_memory_read_backing(uintptr_t, void*, uint64_t) { std::abort(); }
const uint64_t* runtime_memory_generation() { static const uint64_t generation = 0; return &generation; }
void runtime_memory_note_write(uintptr_t, uint64_t) { std::abort(); }
void runtime_memory_set_note_write_hook(void (*)(uintptr_t, uint64_t)) {}
void runtime_memory_set_cpu_write_hook(void (*)(uintptr_t, uint64_t)) {}
void runtime_memory_set_write_watch(uintptr_t, uint64_t, int) { std::abort(); }
void runtime_memory_set_guest_chunk_allocator(int (*)(uint64_t, uint64_t)) {}
void runtime_memory_set_guest_chunk_whole(int) {}
int runtime_memory_vma_info(uintptr_t, int*, int*, uintptr_t*) { std::abort(); }
int runtime_memory_direct_phys(uintptr_t, uint64_t*, uintptr_t*) { std::abort(); }
void runtime_wait_report(double) {}
void runtime_sleep_stats(uint64_t* calls, uint64_t* ns) { *calls = 0; *ns = 0; }
uint64_t runtime_heap_growths() { return 0; }
}

