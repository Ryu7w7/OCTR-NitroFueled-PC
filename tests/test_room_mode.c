#include "../src/runtime_room_mode.c"
#include <assert.h>

void (*runtime_online_mode_chosen)(int, int);
static int speed, own_speed, requests, status = 200, adoptions;
static const char *response = "mode speed";
static char last_request[128];
const char *runtime_np_get_online_name(void) { return "LocalHost"; }
int runtime_public_itemless_announce(void) { return 1; }
void runtime_public_itemless_party_ready(void) {}
int runtime_online_mode_private_speed(void) { return own_speed; }
int runtime_online_mode_in_private(void) { return 1; }
int runtime_speed_mode_enabled(void) { return speed; }
void runtime_speed_mode_set(int enabled) { speed = enabled; }
void runtime_online_mode_adopt(int enabled) { speed = enabled; ++adoptions; }
void runtime_race_options_leave_room(void) {}
void runtime_online_mode_leave_cup(void) {}
int runtime_ryunet_request(const char *method, const char *target, char *body, size_t size) {
    assert(!strcmp(method, "GET") || !strcmp(method, "POST"));
    ++requests;
    snprintf(last_request, sizeof last_request, "%s", target);
    snprintf(body, size, "%s", response);
    return status;
}

static Publication *publication(int value, unsigned generation) {
    Publication *p = malloc(sizeof *p);
    assert(p);
    *p = (Publication){value, generation};
    return p;
}

int main(void) {
    assert(runtime_online_mode_chosen);
    strcpy(followed_host, "RaceHost");
    Follow old = {.host = "RaceHost", .generation = follow_generation};
    status = 503;
    assert(!runtime_room_mode_prepare_race() && !race_mode_frozen && speed == 0);
    status = 200;
    response = "mode speed trailing-data";
    assert(!runtime_room_mode_prepare_race() && !race_mode_frozen);
    response = "mode speed";
    assert(runtime_room_mode_prepare_race() && speed == 1 && race_mode_frozen);
    assert(strstr(last_request, "host=RaceHost"));
    int before = requests;
    response = "mode normal";
    adopt_followed_mode(&old, 0); /* Response issued before loading, delivered afterward. */
    assert(speed == 1);
    for (int race = 1; race < 4; ++race) assert(runtime_room_mode_prepare_race());
    assert(requests == before && speed == 1); /* Entire Cup uses one confirmed mode. */
    runtime_room_mode_race_ended();
    assert(runtime_room_mode_prepare_race() && speed == 0); /* Rematch confirms again. */
    runtime_room_mode_left();
    before = adoptions;
    adopt_followed_mode(&old, 1);
    assert(adoptions == before && speed == 0); /* Old room response cannot affect new room. */

    own_speed = speed = 1;
    status = 503;
    assert(!runtime_room_mode_prepare_race() && !race_mode_frozen);
    status = 200;
    assert(runtime_room_mode_prepare_race());
    assert(strstr(last_request, "name=LocalHost&mode=speed"));
    before = requests;
    publish(publication(0, follow_generation));
    assert(requests == before); /* Queued Normal update cannot undo confirmed Itemless. */
    runtime_room_mode_race_ended();
    publish(publication(0, follow_generation - 1));
    assert(requests == before); /* Out-of-order publication from a previous choice. */
    own_speed = speed = 0;
    assert(runtime_room_mode_prepare_race());
    assert(strstr(last_request, "name=LocalHost&mode=normal"));
    puts("Private room mode confirmation, stale responses, and Cup lifetime tests passed");
}
