/* cc -D_GNU_SOURCE -std=c11 -O2 -ffunction-sections -fdata-sections -Isrc
 * tests/test_live_items.c -Wl,--gc-sections -lm -pthread -o /tmp/test-live-items
 *
 * Item-box rules applied while the game runs (src/runtime_item_lead.c, live_apply): the server's
 * text goes into tables laid out as the game's (CPowerUpData, igHashTables, the race mode lists),
 * an item a table lacks goes into an empty slot, the network config's other table gets the same,
 * and the rules id is kept for the logins. */
#include <assert.h>
#include "../src/runtime_item_lead.c"

static char *fake_system, *fake_network;
void *rr_ptr(size_t offset) {
    if (offset == RR_D_SYSTEM_DATA) return fake_system;
    if (offset == D_NETWORK) return fake_network;
    return NULL;
}
void *rr_handle_object(void **slot) { (void)slot; return NULL; }
void *rr_racer_of(void *kart) { (void)kart; return NULL; }
int runtime_replay_playing(void) { return 0; }
int runtime_net_ping_view(PingPeer *out, int max) { (void)out; (void)max; return 0; }
static char told_id[48];
void runtime_items_id_set(const char *id) { snprintf(told_id, sizeof told_id, "%s", id); }

/* An igHashTable of slots values of step bytes; keys all empty */
static char *table(int slots, int step) {
    char *t = calloc(1, 0x40);
    uint32_t *keys = malloc(4 * (size_t)slots);
    for (int i = 0; i < slots; i++) keys[i] = LIVE_EMPTY;
    RR_AT(t, 0x18, char *) = calloc((size_t)slots, (size_t)step);
    RR_AT(t, 0x20, uint32_t) = 4u * (uint32_t)slots;
    RR_AT(t, 0x28, uint32_t *) = keys;
    return t;
}
static void *put(char *t, int slot, uint32_t key, int step) {
    RR_AT(t, 0x28, uint32_t *)[slot] = key;
    RR_AT(t, 0x30, uint32_t) += 1;
    return RR_AT(t, 0x18, char *) + (size_t)step * slot;
}
static char *list(int n, char **items) {
    char *l = calloc(1, 0x30);
    RR_AT(l, 0xc, int32_t) = n;
    RR_AT(l, 0x20, char **) = items;
    return l;
}
static char *weights(float bomb, float missile) {
    char *t = table(8, 4);
    *(float *)put(t, 0, 0, 4) = bomb;       /* Bomb */
    *(float *)put(t, 3, 1, 4) = missile;    /* Missile; the Super Engine (11) is missing */
    return t;
}

int main(void) {
    char *data = calloc(1, 0x100);
    char *names = table(16, 8);
    *(const char **)put(names, 0, 0, 8) = "Bomb";
    *(const char **)put(names, 1, 1, 8) = "Missile";
    *(const char **)put(names, 2, 5, 8) = "N'Tropy Clock";
    *(const char **)put(names, 3, 11, 8) = "Super Engine";
    RR_AT(data, 0x88, char *) = names;
    char *race[8], *battle[1] = {weights(5, 5)};
    for (int i = 0; i < 8; i++) race[i] = weights(1, 2);
    char *online = table(16, 8);
    *(char **)put(online, 0, 1, 8) = list(8, race);
    *(char **)put(online, 1, 9, 8) = list(1, battle);
    RR_AT(data, 0x18, char *) = online;
    char *cooldowns = table(4, 4);
    *(float *)put(cooldowns, 0, 5, 4) = 40.0f;
    RR_AT(data, 0x20, char *) = cooldowns;
    fake_system = calloc(1, 0x100);
    RR_AT(fake_system, 0xf8, char *) = data;
    /* the network config's online table: another copy (as the online draw reads it) */
    char *race2[8], *battle2[1] = {weights(5, 5)};
    for (int i = 0; i < 8; i++) race2[i] = weights(1, 2);
    char *online2 = table(16, 8);
    *(char **)put(online2, 0, 1, 8) = list(8, race2);
    *(char **)put(online2, 1, 9, 8) = list(1, battle2);
    char *config = calloc(1, 0x200);
    RR_AT(config, 0x138, char *) = online2;
    fake_network = calloc(1, 0x100);
    RR_AT(fake_network, 0x40, char *) = config;

    char text[] = "id abc123\n"
                  "race\t1\t9\tBomb\n"
                  "race\t8\t0\tMissile\n"
                  "race\t2\t3.5\tSuper Engine\n"
                  "battle\t0\t7\tMissile\n"
                  "cooldown\t0\t12\tN'Tropy Clock\n"
                  "race\t1\t4\tNo Such Item\n";
    live_apply(text);
    for (int t = 0; t < 2; t++) {
        char **r = t ? race2 : race;
        char **b = t ? battle2 : battle;
        assert(*(float *)live_slot(r[0], 0, 4) == 9.0f);
        assert(*(float *)live_slot(r[0], 1, 4) == 2.0f);    /* not named: kept */
        assert(*(float *)live_slot(r[7], 1, 4) == 0.0f);
        assert(*(float *)live_slot(r[1], 11, 4) == 3.5f);   /* added in an empty slot */
        assert(RR_AT(r[1], 0x30, uint32_t) == 3);
        assert(!live_slot(r[2], 11, 4));                    /* only where named */
        assert(*(float *)live_slot(b[0], 1, 4) == 7.0f);
    }
    assert(*(float *)live_slot(cooldowns, 5, 4) == 12.0f);
    assert(!strcmp(told_id, "abc123") && !strcmp(LIVE.id, "abc123"));
    /* the same rules again: the added item stays one */
    char again[] = "id abc124\nrace\t2\t1\tSuper Engine\n";
    live_apply(again);
    assert(*(float *)live_slot(race[1], 11, 4) == 1.0f && RR_AT(race[1], 0x30, uint32_t) == 3);
    /* no tables (not loaded yet): nothing written, the id kept */
    RR_AT(fake_system, 0xf8, char *) = NULL;
    char later[] = "id zzz\nrace\t1\t1\tBomb\n";
    live_apply(later);
    assert(!strcmp(told_id, "abc124"));
    puts("live item rules: ok");
    return 0;
}
