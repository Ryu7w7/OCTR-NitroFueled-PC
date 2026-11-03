#include "../src/runtime_time_trial.c"
#include <assert.h>
#include <stdlib.h>

static unsigned char manager[0x60], save[0x20], header[0x30], list[0x28];
static unsigned char entries[4][0x30], ghosts[4][0x30];
static unsigned char *items[4], *no_players;
static void *event_manager;
static const char *paths[] = {"TROPY", "OXIDE", "VELO", "DEV"};
static int emitted, loaded, missing;
static void put_ptr(void *object, size_t offset, const void *value) {
    memcpy((char *)object + offset, &value, sizeof value);
}
static void put_time(void *object, size_t offset, float value) {
    memcpy((char *)object + offset, &value, sizeof value);
}
static unsigned char *ABI find_stub(const char *track) {
    assert(!strcmp(track, "crash_cove"));
    return missing == 5 ? NULL : header;
}
static unsigned char *ABI load_stub(void *self, const char *path) {
    assert(self == manager);
    const char *lower[] = {"tropy", "oxide", "velo", "dev"};
    for (int i = 0; i < 4; ++i) {
        if (!strcmp(path, lower[i])) {
            ++loaded;
            return missing == i ? NULL : ghosts[i];
        }
    }
    abort();
}
static void ABI event_stub(void *self, const char *event, void *player, int a, int b, int c) {
    (void)self;
    assert(!player && !a && !b && c == 1);
    const char *names[] = {"beat_ntropy_ghost", "beat_oxide_ghost", "beat_velo_ghost", "beat_dev_ghost"};
    int tier = integer(save, 0x0c);
    assert(tier >= 0 && tier < 4 && !strcmp(event, names[tier]));
    emitted |= 1 << tier;
}
static void reset(int tier) {
    memset(manager, 0, sizeof manager);
    memset(save, 0, sizeof save);
    memcpy(save + 0x0c, &tier, sizeof tier);
    /* No selected/playing ghost. */
    int selected = -1, count = 4;
    memcpy(manager + 0x34, &selected, 4);
    put_time(manager, 0x38, 200);
    put_time(header, 0x18, 200);
    put_ptr(header, 0x20, list);
    memcpy(list + 0x0c, &count, 4);
    put_ptr(list, 0x20, items);
    for (int i = 0; i < 4; ++i) {
        items[i] = entries[i];
        put_ptr(entries[i], 0x28, paths[i]);
        put_time(ghosts[i], 0x28, 180.0f - 20 * i);
    }
    emitted = loaded = 0;
    missing = -1;
    find_challenges = find_stub;
    load_ghost = load_stub;
    challenge_event = event_stub;
    players = &no_players;
    events = &event_manager;
}
static void finish(float time) { finish_challenges(manager, save, "crash_cove", time); }

/* Exercise the actual installed machine-code bridge against the real executable's signatures,
 * with a tiny finish frame and native-service stubs instead of running/saving a game. */
static void check_image(const char *path) {
    FILE *f = fopen(path, "rb");
    assert(f);
    assert(!fseek(f, 0, SEEK_END));
    long length = ftell(f);
    rewind(f);
    unsigned char *elf = malloc((size_t)length);
    assert(elf && fread(elf, 1, (size_t)length, f) == (size_t)length);
    fclose(f);
    size_t size = 0x3000000;
    unsigned char *image = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(image != MAP_FAILED);
    uint64_t phoff;
    uint16_t count;
    memcpy(&phoff, elf + 32, 8);
    memcpy(&count, elf + 56, 2);
    for (int i = 0; i < count; ++i) {
        unsigned char *p = elf + phoff + i * 56;
        uint64_t off, va, bytes;
        memcpy(&off, p + 8, 8); memcpy(&va, p + 16, 8); memcpy(&bytes, p + 32, 8);
        if (integer(p, 0) == 1) {
            assert(va + bytes <= size && off + bytes <= (uint64_t)length);
            memcpy(image + va, elf + off, bytes);
        }
    }
    free(elf);
    size_t delta = supported(image, size, 0) ? 0 : 16;
    assert(supported(image, size, delta));
    /* Damage a signature: no partial installation is allowed. */
    image[0xfcc890 - delta] ^= 1;
    assert(!supported(image, size, delta));
    assert(runtime_time_trial_start(image, size));
    assert(image[0xfcc602 - delta] == 0x0f && image[0xfcc6d2 - delta] == 0xc5);
    image[0xfcc890 - delta] ^= 1;
    assert(runtime_time_trial_start(image, size));
    assert(image[0xfcc602 - delta] == 0x90);
    /* Cheat rejection and personal-best replacement still use the original branches. */
    assert(!memcmp(image + 0xfcc608 - delta, "\x84\xc0\x0f\x85\x4e\xff\xff\xff", 8));
    assert(!memcmp(image + 0xfcc6c5 - delta, "\x80\x7d\xa8\x00", 4));
    reset(-1);
    unsigned char frame[80];
    const unsigned char prefix[] = {
        0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x58,
        0x49,0x89,0xfd, 0x48,0x89,0x75,0xc0, 0x48,0x89,0x55,0x80,
        0xf3,0x0f,0x11,0x45,0x90, 0xc6,0x45,0xb7,0x01
    };
    memcpy(frame, prefix, sizeof prefix);
    jump_to(frame + sizeof prefix, image + 0xfcc6d2 - delta);
    assert(write_code(image + 0xfcc430 - delta, frame, sizeof prefix + 14));
    const unsigned char epilogue[] = {0x48,0x83,0xc4,0x58,0x5b,0x41,0x5c,0x41,0x5d,0x41,0x5e,0x41,0x5f,0x5d,0xc3};
    assert(write_code(image + 0xfcc8a7 - delta, epilogue, sizeof epilogue));
    typedef void (ABI *Finish)(void *, void *, const char *, float);
    ((Finish)(image + 0xfcc430 - delta))(manager, save, "crash_cove", 119.0f);
    assert(integer(save, 0x0c) == 4 && emitted == 15);
    munmap(image, size);
}

int main(int argc, char **argv) {
    reset(-1); finish(119); assert(integer(save, 0x0c) == 4 && emitted == 15);
    assert(manager[0x30] && integer(manager, 0x34) == -1);
    emitted = loaded = 0; finish(110); assert(!emitted && !loaded);
    reset(-1); finish(170); assert(integer(save, 0x0c) == 1 && emitted == 1);
    reset(-1); finish(190); assert(integer(save, 0x0c) == 0 && !emitted);
    reset(1); finish(139); assert(integer(save, 0x0c) == 3 && emitted == 6);
    reset(-1); finish(200); assert(integer(save, 0x0c) == -1 && !emitted);
    reset(0); finish(180); assert(integer(save, 0x0c) == 0 && !emitted);
    reset(3); finish(120); assert(integer(save, 0x0c) == 3 && !emitted);
    reset(3); finish(119); assert(integer(save, 0x0c) == 4 && emitted == 8);
    reset(0); int selected = 3; memcpy(manager + 0x34, &selected, sizeof selected); finish(139);
    assert(integer(save, 0x0c) == 3 && emitted == 7 && integer(manager, 0x34) == 3);
    reset(-1); missing = 2; finish(100); assert(integer(save, 0x0c) == 2 && emitted == 3);
    reset(-1); missing = 5; finish(100); assert(integer(save, 0x0c) == -1);
    reset(0); put_time(ghosts[0], 0x28, NAN); finish(100); assert(integer(save, 0x0c) == 0);
    reset(0); finish(NAN); finish(INFINITY); finish(0); finish(-1);
    assert(integer(save, 0x0c) == 0 && !loaded);
    reset(-2); finish(100); assert(integer(save, 0x0c) == -2);
    assert(!supported(manager, sizeof manager, 0));
    for (int i = 1; i < argc; ++i) check_image(argv[i]);
    puts("Time Trial progression and bridge checks passed");
}
