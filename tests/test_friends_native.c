#include "../src/runtime_friends_native.c"
#include <assert.h>
#include <stdlib.h>

static unsigned serial;
static int updates;
unsigned runtime_friends_names_serial(void) { return serial; }
static void ABI update_stub(unsigned char *self) { assert(self); ++updates; }

int main(int argc, char **argv) {
    unsigned char manager[0xa8] = {0}, user_data[0x80] = {0};
    unsigned char *user = user_data;
    int player = 2;
    memcpy(user_data + 0x78, &player, sizeof player);
    users = &user;
    native_update = update_stub;
    friends_update(manager);
    assert(updates == 1 && manager[0x52] == 0);

    /* A newly accepted friend queues both list and session refresh for the local user. */
    serial = 1;
    memset(manager + 0x60, 0xff, 4);
    friends_update(manager);
    assert(seen_serial == 1 && manager[0x52] == 1 && manager[0x7a] == 1);
    int status;
    memcpy(&status, manager + 0x60, sizeof status);
    assert(status == 0 && manager[0x50] == 0 && manager[0x51] == 0);
    memset(manager, 0, sizeof manager);
    friends_update(manager);
    assert(manager[0x52] == 0); /* No repeated fetch on unchanged frames. */

    /* Keep a change pending while an older native request is in flight. */
    void *pending = manager;
    memcpy(manager + 0x88, &pending, sizeof pending);
    serial = 2;
    friends_update(manager);
    assert(seen_serial == 1 && manager[0x52] == 0);
    memset(manager + 0x88, 0, sizeof pending);
    friends_update(manager);
    assert(seen_serial == 2 && manager[0x52] == 1);

    memset(manager, 0, sizeof manager);
    serial = 3; /* Removing the last friend must also invalidate the cache. */
    user = NULL;
    friends_update(manager);
    assert(seen_serial == 2);
    user = user_data;
    player = -1;
    memcpy(user_data + 0x78, &player, sizeof player);
    friends_update(manager);
    assert(seen_serial == 2 && manager[0x52] == 0);
    player = 2;
    memcpy(user_data + 0x78, &player, sizeof player);
    friends_update(manager);
    assert(seen_serial == 3 && manager[0x52] == 1 && updates == 8);

    assert(runtime_friends_native_start(manager, sizeof manager));
    if (argc > 1) {
        FILE *f = fopen(argv[1], "rb");
        assert(f);
        fseek(f, 0, SEEK_END);
        size_t size = (size_t)ftell(f);
        rewind(f);
        unsigned char *image = mmap(NULL, size, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        assert(image != MAP_FAILED && fread(image, 1, size, f) == size);
        fclose(f);
        FriendUpdate *slot = (FriendUpdate *)(image + 0x255e468);
        *slot = (FriendUpdate)(image + 0xab55b0);
        assert(runtime_friends_native_start(image, size));
        assert(*slot == friends_update);
        assert(users == (unsigned char **)(image + 0x2ac2e90));
        munmap(image, size);
    }
    puts("Friends native refresh checks passed");
}
