#ifndef BB_RUNTIME_H
#define BB_RUNTIME_H
#include <time.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#ifdef _WIN32
#include "compat_win.h"
/* No signals on Windows: the exception handler resumes the thread in bb_longjmp
 * (gpu/shim/bbport_toggles.h uses the same buffer type). */
typedef bb_jmp_buf sigjmp_buf;
#define sigsetjmp(buffer, save) bb_setjmp(buffer)
#else
#include <setjmp.h>
#endif
/* Recovery point for speculative guest memory reads on this thread (probe.c fault handler). */
extern __thread sigjmp_buf *runtime_fault_recover;
/* Restarts the game (in-game settings menu, render resolution change). */
void runtime_restart(void);
#define ABI __attribute__((sysv_abi))
typedef void (ABI *GuestCallback)(void);
void runtime_start(uint64_t capabilities);
uintptr_t runtime_resolve(const char *name, int is_data);
void runtime_report(void);
void runtime_set_report_hook(void (*hook)(void));
void runtime_lzma_patch(unsigned char *image, size_t size);
void runtime_lzma_report(void);
void runtime_finalize(void *dso);
uintptr_t runtime_mutex_resolve(const char *name);
void runtime_mutex_report(void);
uintptr_t runtime_memory_resolve(const char *name);
void runtime_memory_report(void);
int runtime_memory_is_mapped(uintptr_t address, uint64_t size);
/* The CPU is about to write the range outside guest code (a file read): tells the GPU side. */
void runtime_memory_note_write(uintptr_t address, uint64_t size);
void runtime_memory_note_cpu_write(uintptr_t address, uint64_t size);
/* bbport (frame stats): a guest thread was blocked `ns` in the runtime (0 cond, 1 mutex, 2 sema, 3 sleep). */
void runtime_wait_note(int kind, uint64_t ns);
void runtime_wait_report(double frames);
void runtime_guest_call_sites(uint64_t out[3]);
/* bbport: times the game's heap asked for more memory (posix_mmap): a sign it leaks. */
uint64_t runtime_heap_growths(void);
static inline uint64_t runtime_wait_clock(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec; }
const char *runtime_import_name(const char *name);
uintptr_t runtime_rwlock_resolve(const char *name);
void runtime_rwlock_report(void);
void runtime_set_libc_tls(const void *data, uint64_t filesz, uint64_t memsz);
void runtime_set_module_tls(uint64_t module, const void *data, uint64_t filesz, uint64_t memsz);
void runtime_set_procparam(void *param);
void **runtime_application_heap_api(void);
uintptr_t runtime_thread_resolve(const char *name);
void runtime_thread_report(void);
void runtime_set_main_tls(const void *data, uint64_t filesz, uint64_t memsz, uint64_t align);
void runtime_thread_attach_main(void);
int32_t *runtime_errno(void);
uintptr_t runtime_sema_resolve(const char *name);
uintptr_t runtime_eventflag_resolve(const char *name);
uintptr_t runtime_json2_resolve(const char *name);
void runtime_eventflag_report(void);
void runtime_sema_report(void);
unsigned runtime_sema_waiters(uint32_t id);
uintptr_t runtime_time_resolve(const char *name);
void runtime_content_configure(const uint32_t values[5]);
/* An installed add-on (DLC): its 16-character entitlement label (scripts/content_profile.py). */
void runtime_content_add_addon(const char *label);
uintptr_t runtime_content_resolve(const char *name);
void runtime_content_report(void);
typedef struct { const char *name; void *function; } RuntimeExport;
#define RUNTIME_LOOKUP(table, nid) runtime_lookup(table, sizeof(table)/sizeof(*(table)), nid)
const char *runtime_symbol(const char *nid);
/* SacredFire: imports are named "NID#library" (link_modules.py); a pattern "NID" matches the NID from
 * any library, "NID#library" only from that library. The eboot's own index suffixes never reach here. */
static inline int runtime_nid_equals(const char *name, const char *pattern) {
    const char *h = strchr(name, '#'), *k = strchr(pattern, '#');
    size_t n = h ? (size_t)(h - name) : strlen(name), p = k ? (size_t)(k - pattern) : strlen(pattern);
    if (n != p || memcmp(name, pattern, n)) return 0;
    if (!k) return 1; /* any library */
    const char *library = h ? h + 1 : "", *want = k + 1;
    const char *end = strchr(library, '#');
    size_t l = end ? (size_t)(end - library) : strlen(library);
    return l == strlen(want) && !memcmp(library, want, l);
}
/* Loads "NID library symbol" lines generated for the prepared eboot (tools/gen_import_names.py). */
int runtime_load_import_names(const char *path);
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid);
uintptr_t runtime_kernel_resolve(const char *name);
/* SacredFire: a guest thread is ending: libc's thread destructors (_sceKernelSetThreadDtors) run first. */
void runtime_kernel_thread_exit(void);
uintptr_t runtime_file_resolve(const char *name);
uintptr_t runtime_services_resolve(const char *name);
/* SacredFire: 16:9 videos and cutscenes in ultrawide (runtime_cinematic.c). */
void runtime_cinematic_start(const unsigned char *image);
void runtime_cinematic_video(int delta); /* +1 a video file opened, -1 closed */
int runtime_cinematic_cutscene(void); /* 1 while a cutscene plays (not a race's fly-over) */
void runtime_cinematic_cutscene_fill(void); /* cutscenes get a 16:9 field of view, not a crop */
/* SacredFire: ultrawide title and customization screens (runtime_frontend.c). */
void runtime_frontend_start(const unsigned char *image);
int runtime_online_mode_start(unsigned char *image, size_t size);
int runtime_friends_native_start(unsigned char *image, size_t size);
int runtime_time_trial_start(unsigned char *image, size_t size);
/* The private room's mode through RyuNET (runtime_room_mode.c): the host publishes its choice,
 * a player who joins takes it, also when joining by invitation (no selector seen). */
void runtime_online_mode_adopt(int speed);
void runtime_online_mode_leave_cup(void);
void runtime_online_mode_new_session(void); /* game thread: fresh Online menu choice */
void runtime_online_mode_request_leave(int fresh); /* worker-safe; consumed on game thread */
int runtime_online_mode_private_speed(void);
int runtime_online_mode_itemless_collision(void);
int runtime_online_mode_private_cup(void);
int runtime_online_mode_private_single_options(void);
int runtime_online_mode_private_teams(void);
int runtime_online_mode_in_private(void);
int runtime_online_mode_lobby_open(void);        /* an online lobby is on screen */
void runtime_online_mode_selector_left(void);
void runtime_online_mode_note_frame(void);       /* the game's thread, every frame */
int runtime_online_mode_race_running(void);
int runtime_replay_race_active(void);
/* A private room's member, for the overlay's members list by team (bbport_overlay.cpp mirrors it). */
typedef struct { char name[40]; int team, ready, local; } TeamLobbyMember;
int runtime_online_team_lobby(TeamLobbyMember *out, int max, int *teams);   /* any thread; -1 none */
void runtime_online_team_request(int team);      /* the local player's team, picked in the room (any thread) */
int runtime_online_team_requested(void);         /* that pick, -1 none */
int runtime_online_team_color(int team, unsigned char rgb[3], char *name, size_t size);  /* palette index */
int runtime_online_team_screen(float rects[4][4]);   /* team screen columns to draw (x, y, w on the 1080p canvas); 0 none */
int runtime_online_mode_room_on(void);           /* in_private as of the last frame, for any thread */
int runtime_online_mode_public_on(void);         /* a public match's lobby as of the last frame */
void runtime_load_guard_note(const void *buffer, size_t size); /* the game's stdout/stderr writes: a level load that cannot finish restarts the game (runtime_load_guard.c) */
void runtime_presence_tick(void);                /* the game's thread, every frame: Discord presence facts (runtime_presence.c) */
int runtime_online_mode_private_cup_race(void); /* a race of a private room's CUP is loading or running */
void runtime_room_mode_hosting(void);
void runtime_room_mode_takeover(void);
void runtime_room_mode_joined(const char *session_id);
void runtime_room_mode_left(void);
int runtime_room_mode_following(void);
void runtime_room_mode_host_name(char out[24]);
/* Whether this nickname hosts the room on screen (runtime_room_host.c): the room list's crown. */
int runtime_room_host_is(const char *nick);
int runtime_room_mode_ready(void);
/* 1 verified/frozen, 0 rejected, -1 background verification pending. */
int runtime_room_mode_begin_race(void);
int runtime_room_mode_race_frozen(void);
void runtime_room_mode_end_race(void);
int runtime_public_itemless_start(unsigned char *image, size_t size);
int runtime_public_itemless_available(void);
int runtime_public_itemless_announce(void);
int runtime_public_itemless_active(void);
int runtime_public_itemless_lobby_mode(void);
int runtime_public_match_active(void);
int runtime_public_found_room(void);
int runtime_public_waiting_for_opponent(void);
uint64_t runtime_public_room(void);
uint64_t runtime_public_race_host_pid(void);
void runtime_public_itemless_poll_race_host(void);
int runtime_public_ranked_short(void);
int runtime_public_ranked_min_players(void); /* players a Ranked room needs, as the server said (0 until then: unknown) */
int runtime_public_ranked_clock(int64_t *since_ms, int *players); /* 1: the server's Ranked countdown clock is fresh; since_ms -1 = short of players */
int runtime_online_mode_room_players(void);   /* players in the online room (the game's list) */
int runtime_online_mode_ranked_waiting(void);  /* a Ranked room short of players: countdown held, "WAITING 2/3" shown */
int runtime_private_match_active(void);
int runtime_online_party_active(void);
/* 1 accepted, 0 rejected, -1 asynchronous check pending. */
int runtime_private_party_check(void);
void runtime_private_party_defer_start(void *self, void (ABI *resume)(void *));
int runtime_private_party_update(void);
void runtime_private_party_cancel(void);
int runtime_private_difficulty_modified(void);
int runtime_public_itemless_selected(void);
int runtime_public_itemless_match_active(void);
void runtime_public_itemless_party_left_notice(void);
void runtime_public_itemless_abort_queue(void);
void runtime_public_itemless_poll_ranked_request(void); /* the game's thread: ryunet_ranked.txt (Discord Join) */
int runtime_public_itemless_ranked_request_take(void);   /* Mode Select: the requested Ranked mode once, -1 none */
void runtime_public_itemless_select(int enabled);
int runtime_ranked_active(void);
int runtime_ranked_selected(void);
/* -1 checking, 0 closed/unavailable, 1 open; fresh rechecks before entry. */
int runtime_ranked_status(int fresh);
int runtime_ranked_account(void); /* 1 may enter Ranked, 0 must link Discord or Steam first, -1 not known yet */
/* A ban from Ranked only (the server's ranked_bans.go): 1 banned, its reason and seconds left (0 permanent) */
int runtime_ranked_ban(char *reason, size_t size, long *left_seconds);
/* The Ranked screen's rules notice from the server (runtime_ranked.c): its rules (count), 0 none to show, -1 no
 * answer yet (the built-in notice). kinds: 0 not allowed, 1 penalty, 2 allowed; texts of 1280 bytes. */
int runtime_ranked_rules(char *title, size_t title_size, char *subtitle, size_t subtitle_size, int *kinds,
                         char (*texts)[1280], int max);
void runtime_ranked_gate_update(void *root, int ranked);
/* The confirmed ranked room and its queue (0 Normal, 1 Itemless), 0 not ranked. */
int runtime_ranked_session(uint64_t *room, int *mode);
/* One racer of the validated finish order, best first (runtime_ranked.c). */
/* pos: the on-screen finish position (AIs take their own slots, honestly); time: the stamp of the racer's
 * last required lap as this game recorded it, 0 unknown; self: the local player; laps/stamps: the laps
 * this game recorded for them (the local player's own go to the server as its self-report). */
void runtime_online_mode_racer_left(uint64_t pid);
int runtime_online_mode_departed_kart(void *entity);
struct runtime_ranked_racer { char name[32]; int finished; int pos; int time; int self; int laps; int stamps[7]; int dc; };
/* The finish order of the race just ended, 0 none (runtime_online_mode.c). */
int runtime_online_mode_ranked_order(struct runtime_ranked_racer *out, int max);
/* Ranked results: the worker thread, the report at race end, the lobby card's suffix. */
void runtime_ranked_start(void);
void runtime_ranked_race_end(void);
/* The member's current Ranked tier (1..7), or 0 when unavailable. */
int runtime_ranked_tier(const char *name, int *mode);
int runtime_ranked_badge(const char *name, int *mode); /* -1 for an empty/placeholder slot. */
int runtime_ranked_profile(const char *name, int mode, int *tier, int *mmr, int *placements, int *position);
void runtime_online_mode_ranked_menu_update(unsigned char *root, int shown, int mode);
int runtime_ranked_mmr(const char *name);
const char *runtime_ranked_tier_name(int mode, int tier);
void runtime_online_mode_set_text(void *placeable, const char *text);
void runtime_online_mode_show_placeable(void *placeable, int shown);
void runtime_online_mode_hof_legend(void *project);
int runtime_online_mode_hof_activate(void *project);
void runtime_online_mode_hof_cycle(void *placeable, int direction);
void *runtime_online_mode_content(void *placeable);
void runtime_public_itemless_reset(void);
void runtime_public_itemless_new_session(void);
void runtime_public_itemless_menu_return(void);
void runtime_public_itemless_ranked_return(void);
void runtime_public_itemless_prepare_host_quit(void);
void runtime_public_itemless_begin_race_quit(void);
void runtime_public_itemless_guest_left(const char *session_id);
void runtime_public_itemless_guest_room(int inside);
void runtime_public_itemless_takeover_host(void);
void runtime_public_itemless_party_ready(void);
void runtime_public_itemless_update(void);
int runtime_matchmaking_view(int *elapsed, int *timeout, int *range, int *ranked);
void runtime_online_mode_matchmaking_update(unsigned char *root);
int runtime_online_mode_matchmaking_timeout(void);
int runtime_online_mode_matchmaking_cancel(int route); /* 0 search, 1 room leave, 2 host-closed/Waiting Room */
void runtime_online_mode_set_return_mode(int index); /* 0 Casual, 1 Itemless, 2 Ranked */
int runtime_online_mode_return_mode(void);
int runtime_speed_mode_enabled(void);
int runtime_speed_race_active(void);
void runtime_race_options_leave_room(void);
void runtime_race_options_map_loading(const char *map);
int runtime_race_options_start(unsigned char *image, size_t size);
int runtime_elimination_start(unsigned char *image, size_t size); /* runtime_elimination.c (private rooms' ELIMINATION mode) */
void runtime_elimination_mode_set(int enabled);
int runtime_elimination_ghost_kart(void *entity);   /* a kart out of the race: no collisions, it blinks (any thread) */
int runtime_elimination_mode_enabled(void);
int runtime_elimination_available(void);    /* its hooks are in: the selector offers it */
int runtime_online_mode_private_elimination(void);   /* runtime_online_mode.c: this player's own pick */
int runtime_online_racer_id(void *racer);            /* the host's id of a racer, -1 unknown */
int runtime_online_private_single_race(void);        /* a private room's single race is on */
/* runtime_net.c: the host's list of racers out, in a RYUPN1 packet (kind 6) next to the game's packets */
size_t runtime_elimination_net_send(const char *peer, uint8_t *out, uint64_t now);
int runtime_elimination_net_receive(const char *peer, const uint8_t *in, size_t len, uint64_t now);
void runtime_elimination_tick(void);                             /* the game's thread, every frame */
void runtime_elimination_map_loading(const char *map);
int runtime_elimination_holds_race(void *rules); /* 0xf34430 (runtime_online_mode.c): 1 = the race goes on */
void runtime_elimination_hud_lap(unsigned char *hud); /* 0xe63f20 (runtime_replay_play.c) */
int runtime_elimination_blocks_item(void *controller); /* 0xf0b070 (runtime_replay.c): 1 = refuse */
int runtime_profile_item_guard_start(unsigned char *image, size_t size); /* runtime_profile_guard.c */
void runtime_race_options_stop_boost(void *boost, void (ABI *stop)(void *));
int runtime_itemless_start(unsigned char *image, size_t size);
int runtime_itemless_collisions_disabled(void);
/* N.Sane Mode (runtime_nsane.c, runtime_file.c): a second stored item in private N.Sane races. */
int runtime_nsane_start(unsigned char *image, size_t size);
int runtime_hero_guard_start(unsigned char *image, size_t size);
int runtime_nsane_available(void);
void runtime_nsane_map_loading(const char *map);
void runtime_nsane_tick(void);                   /* the game's thread, every frame: the second slot's roulette */
void runtime_nsane_mode_set(int enabled);
int runtime_nsane_mode_enabled(void);
int runtime_nsane_race_active(void);
int runtime_online_mode_private_nsane(void);
int runtime_replay_start(unsigned char *image, size_t size); /* runtime_replay.c (spectator build) */
/* 1 while a replay of a race without item crates (Time Trial, Itemless) loads or plays (runtime_replay_play.c) */
int runtime_replay_no_crates(void);
/* 1 while a replay or a spectated race has the recorded racer's driver, outfit, kart or look on
 * the player (runtime_replay_play.c, until give_back): a save the game writes then goes to a copy
 * that is thrown away (runtime_savedata.c), not into the player's own. */
int runtime_replay_keeps_save_aside(void);
/* SacredFire online: the running game's title id for the PSN data (runtime_services.c). */
void runtime_np_set_title(const char *title_id);
void runtime_file_report(void);
void runtime_file_configure(const char *app0, const char *user);
int runtime_file_mount(const char *guest, const char *host);
void runtime_file_unmount(const char *guest);
int runtime_file_translate(const char *guest, char *out, size_t size);
int64_t runtime_file_open(const char *path, int flags, int mode);
int64_t runtime_file_close(int fd);
int64_t runtime_file_read(int fd, void *buffer, uint64_t size);
int64_t runtime_file_pread(int fd, void *buffer, uint64_t size, int64_t offset);
int64_t runtime_file_write(int fd, const void *buffer, uint64_t size);
int64_t runtime_file_lseek(int fd, int64_t offset, int whence);
int64_t runtime_file_stat(const char *path, void *guest_stat);
int64_t runtime_file_fstat(int fd, void *guest_stat);
int64_t runtime_file_getdents(int fd, char *buffer, uint64_t size, int64_t *base);
void runtime_thread_keys_cleanup(void);
/* Guest-visible errno values are FreeBSD's. */
int32_t runtime_guest_errno(int host_errno);
void *runtime_low_map(size_t size, int prot);
/* Windows: reserves the PS4 user address range as a placeholder (no-op elsewhere). */
void runtime_memory_reserve(void);
uintptr_t runtime_ajm_resolve(const char *name);
void runtime_ajm_report(void);
uintptr_t runtime_audio_resolve(const char *name);
void runtime_audio_report(void);
uintptr_t runtime_pad_resolve(const char *name);
void runtime_pad_report(void);
/* SacredFire: controller ports for split screen. Port 1 is the player's pad (keyboard too); ports
 * 2..4 take the gamepad named in bbport.ini (port.N=<GUID or part of the name>, written by the
 * in-game menu) and appear to the game as logged-in users 2..4 while that gamepad is connected. */
int runtime_pad_user_present(int user);
uint32_t runtime_pad_host_buttons(void);   /* runtime_pad.c: the first port's BTN_* bits at its last read */
uint32_t runtime_pad_host_right_stick(void);   /* runtime_pad.c: right stick x | y << 8 (128 centred) */
void runtime_services_user_change(int user, int login);
uintptr_t runtime_rtc_resolve(const char *name);
const char *runtime_file_user_dir(void);
const char *runtime_file_progress_dir(void); /* saves and trophies: BB_PROGRESS_DIR or the user folder */
/* Pack Tacho's reserves bar starts off (runtime_reserves_bar.c). */
void runtime_reserves_bar_start(unsigned char *image, size_t size);
void runtime_reserves_bar_save_closed(void);  /* the game closed its save (runtime_savedata.c) */
void runtime_reserves_bar_tick(void);         /* the game's thread, every frame */
/* Voice chat (gpu/shim/bbport_voice.cpp): the race's karts with their players' names, as of the
 * last frame (runtime_item_lead.c); 0 outside a race. here: a player of this PC. */
/* effects: what an item does to that kart, for its voice (runtime_item_lead.c): the bubble shield (muffled),
 * N. Tropy's clock (lower, slowed), Invisibility (an echo), a TNT on the head (covered, as a hand over the mic)
 * and squashed (the same, much stronger: nothing understood) */
enum { RUNTIME_VOICE_BUBBLE = 1, RUNTIME_VOICE_CLOCK = 2, RUNTIME_VOICE_INVISIBLE = 4, RUNTIME_VOICE_TNT = 8, RUNTIME_VOICE_CRUSHED = 16 };
struct RuntimeVoiceKart { char name[24]; float pos[3]; int here; int effects; };
int runtime_voice_karts(struct RuntimeVoiceKart *out, int max);
/* Where the game shows each racer's nickname over its kart (HUD kart markers, in the game's
 * 1920x1080 UI units), copied a few times a second during a race (runtime_replay_play.c). */
struct RuntimeVoiceMarker { char name[24]; float x, y, scale; int shown; };
void runtime_voice_markers_tick(int racing);
/* 1 while that player is heard (or this player talks): gpu/shim/bbport_voice.cpp. */
int runtime_voice_speaking(const char *nick);
/* The party's players by name, as of the lobby's last frame (runtime_public_itemless.c). */
int runtime_room_members(char out[][24], int max);
int runtime_voice_markers(struct RuntimeVoiceMarker *out, int max);
/* 1 when the game's fonts have the loudspeaker character (BB_VOICE_GLYPH=1, scripts/button_prompts.py): the
 * nickname over a talking racer's kart then ends with it. */
int runtime_voice_glyph(void);
/* The port's own texts in the game's language (src/runtime_text.c, scripts/octr_texts.py): the
 * translation of an English text, or the text itself when it has none. runtime_text_is: a shown
 * text is that English text or its translation. */
const char *runtime_text(const char *english);
int runtime_text_is(const char *shown, const char *english);
int runtime_text_column(void);
void runtime_text_set_language(int id);
/* Host handoff research (runtime_party_experiment.c): Ctrl+F9 / Ctrl+Shift+F9 / Ctrl+F10 in a lobby. */
int runtime_party_experiment_start(unsigned char *image, size_t size);
void runtime_party_experiment_request(int kind); /* any thread: 1 leave+host, 2 close+host, 3 join */
int runtime_party_hosting(void);                 /* this player hosts its party */
void runtime_party_host_lost(int role, int private_match, int announced_return); /* old host connection was torn down */
void runtime_party_experiment_tick(void);          /* the game's thread, in an online lobby */
/* Server-backed private-room parties (opt-in BB_PARTY_SERVER=1, runtime_party_server.c). */
void runtime_party_server_init(void);
int runtime_party_server_enabled(void);
int runtime_party_server_create(const char *sid);         /* host: create on server */
int runtime_party_server_join(const char *sid);           /* guest: join on server */
int runtime_party_server_promote(const char *successor);  /* host: promote before quit */
void runtime_party_server_tick(void);                     /* guest: poll for host change */
void runtime_party_server_defer_promote(const char *successor); /* host: promote after a race ends */
void runtime_party_server_matchmaking(int i_am_host, const char *host_name); /* shared session for a queue */
void runtime_party_server_matchmaking_leave(int keep_party);
void runtime_savedata_configure(const char *title);
const char *runtime_savedata_title(void);
/* The game's TITLE_ID: where its trophies are kept (<user>/trophies/<title>/). */
void runtime_trophy_configure(const char *title);
void runtime_meta_dump_start(unsigned char *image, size_t size); /* BB_META_DUMP=1: field metadata (lab) */
void runtime_trophy_badges(const char *json); /* the server's badges: OCTR's trophies (ctr_trophies.h) */
uintptr_t runtime_savedata_resolve(const char *name);
void runtime_savedata_report(void);
void runtime_thread_attach_host(const char *name);

/* Notify the retained native leaderboard graph after the HOF view is initialized. */
void runtime_hof_native_ready(void *project);
void runtime_hof_focus_list(void *placeable);
#endif
