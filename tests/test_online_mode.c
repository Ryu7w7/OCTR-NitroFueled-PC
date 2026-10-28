#define _GNU_SOURCE
#include "runtime.h"
#include <assert.h>
#include <stdlib.h>
#include <sys/mman.h>

static int speed = -1, private = 1, base_calls;
static int room_active;
void runtime_speed_mode_set(int enabled) { speed = enabled; }
int runtime_speed_race_active(void) { return speed == 1 && room_active; }
static void ABI private_state(unsigned char *yes, unsigned char *no) {
    *yes = (unsigned char)private; *no = (unsigned char)!private;
}
static void ABI base_event(void *self, void *event, void *owner) {
    (void)self; (void)event; (void)owner; base_calls++;
}
static void *ABI event_type(void *event) {
    int index; memcpy(&index, (unsigned char *)event + 8, 4);
    return (void *)(uintptr_t)(index == -1 ? 123 : 456);
}
static void (ABI *choice)(void *, int);
static void ABI original_handler(void *self, void *event, void *owner) {
    (void)self;
    int index; memcpy(&index, (unsigned char *)event + 8, 4);
    choice(owner, index);
}
static void stub(unsigned char *at, void *function) {
    /* Preserve the signature used by the installer, then tail-call the mock. */
    memcpy(at, "\x55\x48\x89\xe5\x41\x57\x41\x56\x41\x5e\x41\x5f\x5d\x48\xb8", 15);
    memcpy(at + 15, &function, 8);
    memcpy(at + 23, "\xff\xe0", 2);
}
int main(void) {
    const size_t size = 0x3000000;
    unsigned char *image = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(image != MAP_FAILED);
    unsetenv("BB_ONLINE_MODE_HOOK");
    assert(runtime_online_mode_start(image, size));
    setenv("BB_ONLINE_MODE_HOOK", "1277e00", 1);
    assert(!runtime_online_mode_start(image, size));
    memcpy(image + 0x1277e00, "\x55\x48\x89\xe5\x41\x57\x41\x56\x53\x50\x48\x8d\x05\x97\xa1\x87\x01", 17);
    /* Record the remapped index and relocated LEA result, then undo the prologue. */
    memcpy(image + 0x1277e11, "\x89\x37\x48\x89\x47\x08\x48\x83\xc4\x08\x5b\x41\x5e\x41\x5f\x5d\xc3", 17);
    stub(image + 0x125d570, (void *)base_event);
    stub(image + 0xf50350, (void *)private_state);
    stub(image + 0x1277c70, (void *)original_handler);
    void *handler = image + 0x1277c70;
    memcpy(image + 0x2773b40, &handler, 8);
    unsigned char state[0x148] = {0};
    void *ptr = state;
    memcpy(image + 0x2acb3d8, &ptr, 8);
    ptr = (void *)123;
    memcpy(image + 0x2a21ee8, &ptr, 8);
    /* Missing/mismatched seasonal code must reject installation before any writes. */
    assert(!runtime_online_mode_start(image, size));
    memcpy(image + 0xfb7710, "\x55\x48\x89\xe5\x41\x57\x41\x56\x41\x54\x53\x48\x83\xec\x20", 15);
    memcpy(image + 0xfb771f, "\x89\x37\x48\x83\xc4\x20\x5b\x41\x5c\x41\x5e\x41\x5f\x5d\xc3", 15);
    assert(runtime_online_mode_start(image, size));
    void (ABI *season)(void *, int) = (void (ABI *)(void *, int))(image + 0xfb7710);
    choice = (void (ABI *)(void *, int))(image + 0x1277e00);
    uintptr_t result[2] = {0};
    unsigned char self[0x1d0] = {0}, options[40] = {0}, flags[40] = {0}, items[3] = {1,1,1};
    ptr = options; memcpy(self + 0x1a8, &ptr, 8);
    ptr = flags; memcpy(self + 0x1b0, &ptr, 8);
    ptr = items; memcpy(flags + 0x20, &ptr, 8);
    const int count = 3; memcpy(options + 12, &count, 4);
    void *vtable[] = {(void *)event_type};
    struct { void **vtable; int index; } event = {vtable, -1};
    void (ABI *handle)(void *, void *, void *);
    memcpy(&handle, image + 0x2773b40, sizeof handle);
    private = 0;
    handle(self, &event, NULL);
    assert(base_calls == 1 && items[1] == 0 && speed == 0);
    private = 1;
    handle(self, &event, NULL);
    assert(base_calls == 2 && items[1] == 1);
    event.index = 1; handle(self, &event, result);
    assert(speed == 1 && result[0] == 0 && result[1] == (uintptr_t)(image + 0x2af1fa8));
    event.index = -1; handle(self, &event, NULL);
    int selected; memcpy(&selected, self + 0x190, 4);
    assert(selected == 1 && speed == 1); /* Return from the private lobby. */
    private = 0; handle(self, &event, NULL);
    memcpy(&selected, self + 0x190, 4);
    assert(selected == 0 && speed == 0 && items[1] == 0);
    private = 1; handle(self, &event, NULL);
    memcpy(&selected, self + 0x190, 4);
    assert(selected == 1 && speed == 1); /* Public navigation doesn't erase private choice. */
    event.index = 2; handle(self, &event, result);
    assert(speed == 0 && result[0] == 1);
    event.index = 0; handle(self, &event, result);
    assert(speed == 0 && result[0] == 0);
    private = 0;
    event.index = 1; handle(self, &event, result);
    assert(speed == 0 && result[0] == 0);
    /* Other native two-option selectors retain their original index meanings. */
    const int ordinary_count = 2; memcpy(options + 12, &ordinary_count, 4);
    handle(self, &event, result);
    assert(speed == 0 && result[0] == 1);
    for (int theme = 0; theme <= 2; ++theme) {
        int applied = -1;
        speed = 0; room_active = 1;
        season(&applied, theme); assert(applied == theme);
        speed = 1;
        season(&applied, theme); assert(applied == 0);
        /* Remembered Speed must not suppress Adventure/Arcade/public seasons. */
        room_active = 0;
        season(&applied, theme); assert(applied == theme);
        /* Switching back to Normal restores the theme without a restart. */
        speed = 0; room_active = 1;
        season(&applied, theme); assert(applied == theme);
    }
    munmap(image, size);
}
