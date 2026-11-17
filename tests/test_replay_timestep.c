/* cc -D_GNU_SOURCE -std=c11 -O2 -ffunction-sections -fdata-sections -Isrc
 * tests/test_replay_timestep.c -Wl,--gc-sections -lm -pthread -o /tmp/test-replay */
#include "../src/runtime_replay_play.c"
#include "../src/runtime_text.c"
void runtime_nsane_tick(void) {}
void runtime_reserves_bar_tick(void) {}
#include <assert.h>

static _Alignas(16) char frame_data[0x40], hud_data[0x100], project_data[0x100];
static _Alignas(16) char damage_data[0x40], move_data[0x300], controller_data[0x100], racer_data[0x200];
static int ghost_calls, loads, damage_calls, test_items;
unsigned char *rr_game(size_t offset) { return (unsigned char *)(uintptr_t)offset; }
void *rr_ptr(size_t offset) {
    if (offset == RR_D_FRAME) return frame_data;
    if (offset == D_HUD_META) return hud_data;
    if (offset == RR_D_DAMAGE_META) return damage_data;
    if (offset == RR_D_RACER_MANAGER) return project_data;
    return NULL;
}
void *rr_find_component(void *entity, void *meta) {
    (void)entity;
    return meta == hud_data || meta == damage_data ? meta : NULL;
}
void *rr_handle_object(void **slot) { return *slot; }
void *rr_kart_of(int i, int *id) { (void)id; return i == 0 ? project_data : NULL; }
void *rr_component(void *kart, int meta) {
    if (!kart || !test_items) return NULL;
    if (meta == RR_D_DAMAGE_META) return damage_data;
    if (meta == RR_D_KART_CONTROLLER_META) return move_data;
    return meta == RR_D_POWER_UP_META ? controller_data : NULL;
}
int rr_item_type(void *handle) { (void)handle; return -1; }
void *rr_world(void) { return NULL; }
uint32_t rr_world_id(void) { return 0; }
void *rr_game_data(int list, const char *name) { (void)list; (void)name; return NULL; }
const char *rr_current_level(void) { return NULL; }
void rr_race_dir(char *out, size_t size) { snprintf(out, size, "/tmp"); }
void runtime_online_mode_note_frame(void) {}
void runtime_presence_tick(void) {}
void runtime_tips_tick(void) {}
void runtime_item_lead_tick(int racing_now) { (void)racing_now; }
int rr_unpack_frame(struct rr_frame *f, const struct rr_frame *p1, const struct rr_frame *p2,
                    const uint8_t **in, const uint8_t *end, int version) {
    (void)f; (void)p1; (void)p2; (void)in; (void)end; (void)version;
    assert(!"this timing test must not load a replay file");
    return 0;
}
static void *ABI ghost_event(void *pool) { (void)pool; ghost_calls++; return NULL; }
static void *ABI tag_event(int pool) { (void)pool; return NULL; }
static void ABI load_map(void) { loads++; }
static void *ABI racer_of(void *manager, void *kart) { (void)manager; (void)kart; return racer_data; }
static int ABI take_damage(void *damage, int type, void **attacker, int flag) {
    (void)damage; (void)type; (void)attacker; (void)flag;
    damage_calls++;
    return 1;
}

static void reset(void) {
    P.pending = P.started = P.paused = P.give_back = 0;
    P.step = STEP_NONE;
    P.wait = P.frames_waited = P.unghost_frames = P.back_frames = 0;
    P.cast_in = P.last_frame = 0;
    P.kart = project_data;
    memset(P.attached, 0, sizeof P.attached);
    P.attached[0] = 1;
    P.kart_slot[0] = 0;
    memset(frame_data, 0, sizeof frame_data);
    RR_AT(hud_data, 0x38, void *) = project_data;
    game.ghost_look_end_new = ghost_event;
    game.load_next_map = load_map;
    TAGS_SPAWN = tag_event;
    tags_reset();
    IL.setting = 0;
    loads = ghost_calls = 0;
    damage_calls = test_items = 0;
}

static void tick(float dt) {
    RR_AT(frame_data, 0x18, float) = dt;
    RR_AT(frame_data, 0x20, int32_t)++;
    runtime_replay_tick();
}

static void recording_windows(int rate) {
    reset();
    P.header.go_time = 0;
    P.header.racer[0].kind = RR_REMOTE;
    P.count = 14 * rate + 1;
    P.frames = calloc(P.count, sizeof *P.frames);
    assert(P.frames);
    P.watch = 0;
    P.pending = 2;
    P.started = 1;
    for (int r = 1; r < RR_RACERS; ++r) P.kart_slot[r] = -1;
    for (uint32_t i = 0; i < P.count; ++i) {
        struct rr_frame *f = &P.frames[i];
        f->time = (float)i / rate;
        f->racer[0].item = -1;
        f->racer[0].velocity[0] = f->time < 2 ? 4000 : 8000;
        f->racer[0].item_uses = f->time >= 2;
        f->racer[0].buttons = f->racer[0].ghost_bits = 1;
        f->racer[0].steer = 100;
        f->racer[0].position[0] = (int)i * 100;
    }
    P.frames[rate * 2].racer[0].damage = 1;
    P.time = 2.4f;
    assert(pad_bits() & (1u << 6));
    P.time = 2.6f;
    assert(!(pad_bits() & (1u << 6)));
    P.frames[10 * rate].racer[0].item = 2;
    assert(next_item(0, 0) == 2);
    P.frames[10 * rate].racer[0].item = -1;
    P.frames[rate * 25 / 2].racer[0].item = 2;
    assert(next_item(0, 0) == -1);
    P.frames[rate * 25 / 2].racer[0].item = -1;
    IT.ready = 1;
    const float near[] = {1.4f, 2.6f}, far[] = {1.1f, 2.9f};
    for (unsigned i = 0; i < 2; ++i) {
        IT.hit_used[0] = 0;
        P.time = near[i];
        assert(runtime_replay_take_damage(damage_data, 0));
        IT.hit_used[0] = 0;
        P.time = far[i];
        assert(!runtime_replay_take_damage(damage_data, 0));
    }
    P.time = 2.5f;
    P.speed_clock = 1;
    P.shown_speed = 0;
    speed_now(0);
    assert(abs(P.shown_speed - 54) <= 2); /* one-second time window, not 30 samples */
    test_items = 1;
    game.racer_of = racer_of;
    IT.take_damage = take_damage;
    IT.hit_used[0] = IT.hit_frame[0] = 0;
    IT.last = 2 * rate - 1;
    IT.slot_kart = P.kart;
    P.time = 2;
    items_frame();
    assert(IT.hit_frame[0] == (uint32_t)(2 * rate));
    P.time = 2.65f;
    items_frame();
    assert(damage_calls == 0);
    P.time = 2.75f;
    items_frame();
    assert(damage_calls == 1);
    /* Remote input cleanup uses duration even when the recording sample density changes. */
    for (uint32_t i = 0; i < P.count; ++i) {
        const float t = P.frames[i].time;
        if (t >= 1 && t < 2.5f) P.frames[i].racer[0].position[0] = 0;
        if (t >= 4 && t < 4.5f) P.frames[i].racer[0].position[0] = 0;
        if ((t >= 5 && t < 5.1f) || (t >= 6 && t < 6.3f))
            P.frames[i].racer[0].buttons = P.frames[i].racer[0].ghost_bits = 0;
    }
    clean(0);
    assert(P.frames[rate * 2].racer[0].velocity[0] == 0);
    assert(P.frames[rate * 4].racer[0].velocity[0] != 0);
    assert(P.frames[rate * 5].racer[0].buttons == 1);
    assert(P.frames[rate * 6].racer[0].buttons == 0);
    free(P.frames);
    P.frames = NULL;
    P.count = 0;
    IT.ready = 0;
}

int main(void) {
    const int rates[] = {30, 60, 72, 90, 120, 144, 240};
    for (unsigned r = 0; r < sizeof rates / sizeof *rates; ++r) {
        const int fps = rates[r];
        reset();
        P.started = 1;
        P.paused = 1; /* maintenance cadence does not follow the replay playhead */
        for (int i = 1; i <= fps * 8; ++i) {
            unghost(1.f / fps);
            nicknames(1.f / fps);
            assert(ghost_calls == i / (fps * 2));
            assert(TAGS.tries == (i * 2 / fps < 13 ? i * 2 / fps : 13));
        }
        reset();
        P.step = STEP_LOAD;
        P.wait = 90;
        for (int i = 0; i < fps * 3; ++i) {
            tick(1.f / fps);
            assert(loads == 0);
            const double before = P.wait;
            runtime_replay_tick(); /* duplicate pad read must not advance the timer */
            assert(P.wait == before);
        }
        tick(1.f / fps);
        assert(loads == 1 && P.step == STEP_NONE);
        reset();
        P.step = STEP_KART;
        P.wait = 3;
        for (int i = 0; i < 3; ++i) tick(1.f / fps);
        assert(P.wait == 0 && P.frames_waited == 0); /* lifecycle settling stays frame based */
        int waited = 0;
        while (P.step == STEP_KART && waited <= fps * 21) tick(1.f / fps), waited++;
        assert(waited >= fps * 20 && waited <= fps * 20 + 1);
        assert(P.step == STEP_LOAD && P.wait == 90);
    }
    reset();
    P.started = 1;
    for (int i = 0; i < 960; ++i) {
        const float dt = i % 2 ? 1.f / 80 : 1.f / 240;
        unghost(dt);
        nicknames(dt);
    }
    assert(ghost_calls == 4 && TAGS.tries == 13);
    const float fallback[] = {0, -.1f, NAN, INFINITY, 1.f / 20};
    for (unsigned i = 0; i < sizeof fallback / sizeof *fallback; ++i)
        assert(replay_frame_step(fallback[i]) == 1);
    recording_windows(30);
    recording_windows(60);
    recording_windows(120);
    recording_windows(240);
    puts("replay timestep: ok");
}
