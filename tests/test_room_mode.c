#include "../src/runtime_room_mode.c"
#include <assert.h>
#include <unistd.h>

void (*runtime_online_mode_chosen)(int, int);
static int active_speed;
void runtime_speed_mode_set(int enabled) { active_speed = enabled; }
int runtime_speed_mode_enabled(void) { return active_speed; }
int runtime_online_mode_private_speed(void) { return 0; }
int runtime_online_mode_in_private(void) { return 0; }
void runtime_race_options_leave_room(void) {}
void runtime_online_mode_leave_cup(void) {}
void runtime_online_mode_adopt(int speed) { active_speed = speed; }
int runtime_public_itemless_announce(void) { return 1; }
void runtime_public_itemless_party_ready(void) {}
const char *runtime_np_get_online_name(void) { return "TestHost"; }

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static int requests, release_first, last_speed;
static int get_status = 200;
static const char *get_body = "mode speed\n";
static int leave_during_get, block_get, entered_get;
int runtime_ryunet_request(const char *method, const char *target, char *body, size_t size) {
    if (!strcmp(method, "GET")) {
        pthread_mutex_lock(&lock);
        entered_get = 1;
        pthread_cond_broadcast(&condition);
        while (block_get) pthread_cond_wait(&condition, &lock);
        pthread_mutex_unlock(&lock);
        assert(strstr(target, "/ryunet/room-mode?host=TestGuestRoom&format=text") ||
               strstr(target, "/ryunet/room-mode?host=TestHost&format=text"));
        if (leave_during_get) runtime_room_mode_left();
        snprintf(body, size, "%s", get_body);
        return get_status;
    }
    assert(!strcmp(method, "POST"));
    pthread_mutex_lock(&lock);
    ++requests;
    pthread_cond_broadcast(&condition);
    while (requests == 1 && !release_first) pthread_cond_wait(&condition, &lock);
    last_speed = strstr(target, "mode=speed") != NULL;
    pthread_mutex_unlock(&lock);
    snprintf(body, size, "status ok\n");
    return 200;
}

static void test_publish_order(void) {
    publish_mode(0);
    pthread_mutex_lock(&lock);
    while (!requests) pthread_cond_wait(&condition, &lock);
    pthread_mutex_unlock(&lock);
    for (int i=0; i<20; ++i) publish_mode(i % 2);
    pthread_mutex_lock(&lock);
    release_first = 1;
    pthread_cond_broadcast(&condition);
    pthread_mutex_unlock(&lock);
    for (int i=0; i<1000; ++i) {
        pthread_mutex_lock(&publish_lock);
        int running = publish_running;
        pthread_mutex_unlock(&publish_lock);
        if (!running) {
            assert(requests == 2 && last_speed == 1);
            puts("Room mode: delayed writes preserve latest selection");
            assert(runtime_room_mode_ready());
            return;
        }
        const struct timespec delay = {0, 1000000};
        nanosleep(&delay, NULL);
    }
    assert(!"publisher did not finish");
}

static int await_mode(void) {
    int result=runtime_room_mode_begin_race();
    for (int i=0;result<0 && i<2000;++i) {
        const struct timespec delay={0,1000000}; nanosleep(&delay,NULL);
        result=runtime_room_mode_begin_race();
    }
    assert(result>=0); return result;
}
int main(void) {
    test_publish_order();
    active_speed = 1;
    alarm(5);
    block_get = 1;
    assert(runtime_room_mode_begin_race() == -1);
    pthread_mutex_lock(&lock);
    while (!entered_get) pthread_cond_wait(&condition, &lock);
    pthread_mutex_unlock(&lock);
    for (int i=0; i<100; ++i) assert(runtime_room_mode_begin_race() == -1);
    pthread_mutex_lock(&lock);
    block_get = 0;
    pthread_cond_broadcast(&condition);
    pthread_mutex_unlock(&lock);
    assert(await_mode());
    runtime_room_mode_end_race();
    alarm(0);
    get_body = "mode normal\n";
    assert(!await_mode()); /* Server forgot a host's Itemless publication. */
    get_body = "mode speed\n";
    assert(await_mode());
    runtime_room_mode_end_race();
    strcpy(followed_host, "TestGuestRoom");
    Follow peer = {.generation = follow_generation};
    assert(runtime_room_mode_following());
    follow_update(&peer, 1);
    assert(active_speed == 1);
    active_speed = 0; /* A local UI reset must be repaired even without a host change. */
    follow_update(&peer, 1);
    assert(active_speed == 1);

    get_status = 503;
    assert(!await_mode() && !race_frozen);
    get_status = 200; get_body = "mode speed stale";
    assert(!await_mode() && !race_frozen);
    get_body = "mode normal\n";
    assert(await_mode() && race_frozen && active_speed == 0);
    follow_update(&peer, 1); /* Response from before the final snapshot arrives late. */
    assert(active_speed == 0);
    get_status = 503;
    assert(await_mode()); /* CUP track two retains the frozen rules. */
    runtime_room_mode_joined("cusa13795-session-TestGuestRoom");
    assert(race_frozen && following(&peer)); /* Repeated session info isn't a new room. */
    runtime_room_mode_end_race();
    follow_update(&peer, 1);
    assert(active_speed == 1);
    get_status = 200; leave_during_get = 1;
    assert(!await_mode()); /* Leaving during verification invalidates its reply. */
    leave_during_get = 0;
    runtime_room_mode_left();
    follow_update(&peer, 1); /* A response from the old room cannot alter the new one. */
    assert(active_speed == 0 && !runtime_room_mode_following() && !race_frozen);
    puts("Room mode: verified loading, frozen CUP rules, stale responses and room leave passed");
}
