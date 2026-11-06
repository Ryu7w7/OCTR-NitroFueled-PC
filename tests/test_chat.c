/* The chat's client side, without a server or a game. */
#include "../src/runtime_chat.c"
#include <assert.h>

static char dir[256], requested[600], posted[RC_SEND];
static int room_on_now = 1, public_now = 0, following = 0, reply_status = 200, peers_now = 0;
static const char *reply_text = "ok 0\n";

int runtime_online_mode_room_on(void) { return room_on_now; }
int runtime_online_mode_public_on(void) { return public_now; }
int runtime_replay_chat_allowed(void) { return 1; }
int runtime_room_mode_following(void) { return following; }
void runtime_room_mode_host_name(char out[24]) { snprintf(out, 24, "%s", following ? "TheHost" : "Me_1"); }
const char *runtime_np_get_online_name(void) { return "Me_1"; }
const char *runtime_file_progress_dir(void) { return dir; }
int runtime_net_ping_view(PingPeer *out, int max) {
    static const char *names[] = {"Guest_1", "", "bad name", "Guest_2"};
    int n = 0;
    for (; n < peers_now && n < max; n++) snprintf(out[n].name, sizeof out[n].name, "%s", names[n]);
    return n;
}
int runtime_ryunet_transfer(const char *method, const char *target, const void *data, size_t size, char **reply, size_t *reply_size,
                            size_t max_reply) {
    (void)max_reply;
    snprintf(requested, sizeof requested, "%s %s", method, target);
    posted[0] = 0;
    if (size) memcpy(posted, data, size), posted[size] = 0;
    *reply = strdup(reply_text);
    *reply_size = strlen(reply_text);
    return reply_status;
}

static int view(RcLine *out, int *status) {
    int count = 0;
    runtime_chat_view(out, RC_LINES, &count, status);
    return count;
}

int main(void) {
    static RcLine out[RC_LINES];
    int status = 0;
    snprintf(dir, sizeof dir, "%s", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    char path[800];
    muted_path(path, sizeof path);
    remove(path);

    /* the first question has no since (nothing said before arrives); the host's says who is in the room */
    assert(in_room(room) == 1 && !strcmp(room, "Me_1"));
    peers_now = 4;
    poll_room("Me_1");
    assert(!strcmp(requested, "GET /ryunet/chat?name=Me_1&room=Me_1&roster=Guest_1,Guest_2"));
    assert(have_since && since == 0);
    poll_room("Me_1");
    assert(!strcmp(requested, "GET /ryunet/chat?name=Me_1&room=Me_1&since=0&roster=Guest_1,Guest_2"));
    /* a guest does not */
    following = 1;
    in_room(room);
    poll_room("Me_1");
    assert(!strcmp(requested, "GET /ryunet/chat?name=Me_1&room=TheHost&since=0"));

    char reply[] = "ok 3\n1\tGuest_1\thola que tal\n2\tGuest_2\tbuenas\nbroken line\n3\tbad name\tno\n";
    apply_reply(reply);
    assert(since == 3);
    assert(view(out, NULL) == 2);
    assert(!strcmp(out[0].nick, "Guest_1") && !strcmp(out[0].text, "hola que tal"));
    assert(!strcmp(out[1].text, "buenas") && out[1].id == 2);

    /* a muted player's lines go, and stays muted in the next game */
    runtime_chat_mute("GUEST_1", 1);
    assert(runtime_chat_muted("guest_1"));
    assert(view(out, NULL) == 1 && !strcmp(out[0].nick, "Guest_2"));
    muted_count = 0;
    muted_loaded = 0;
    assert(runtime_chat_muted("Guest_1"));
    runtime_chat_mute("Guest_1", 0);
    assert(!runtime_chat_muted("Guest_1") && view(out, NULL) == 2);

    /* the oldest lines give way */
    for (unsigned i = 0; i < RC_LINES + 5; i++) add_line(100 + i, "Guest_1", "x");
    assert(view(out, NULL) == RC_LINES && out[RC_LINES - 1].id == 100 + RC_LINES + 4 && out[0].id == 105);

    /* sending */
    assert(runtime_chat_send("hi") && runtime_chat_send("bye"));
    flush_outbox("Me_1");
    assert(!strcmp(requested, "POST /ryunet/chat?name=Me_1&room=TheHost") && !strcmp(posted, "bye"));
    assert(runtime_chat_send("hello"));
    reply_status = 429;
    flush_outbox("Me_1");
    assert(!strcmp(requested, "POST /ryunet/chat?name=Me_1&room=TheHost") && !strcmp(posted, "hello"));
    view(out, &status);
    assert(status == RC_STATUS_WAIT);
    reply_status = 200;

    runtime_chat_set_enabled(0);
    assert(!runtime_chat_send("off"));
    runtime_chat_set_enabled(1);
    for (int i = 0; i < OUTBOX; i++) assert(runtime_chat_send("full"));
    assert(!runtime_chat_send("one too many"));

    /* leaving the room forgets it, and what was queued */
    leave_room("");
    assert(view(out, NULL) == 0 && !have_since && !outbox_count && !room[0]);
    room_on_now = 0;
    assert(!runtime_chat_available());

    /* a public lobby: no roster, the server says who is in, and the panel waits for its yes */
    public_now = 1;
    assert(in_room(room) == 1 && !strcmp(room, PUBLIC_ROOM));
    assert(!runtime_chat_available());
    poll_room("Me_1");
    assert(!strcmp(requested, "GET /ryunet/chat?name=Me_1&public=1"));
    assert(runtime_chat_available());
    poll_room("Me_1");
    assert(!strcmp(requested, "GET /ryunet/chat?name=Me_1&public=1&since=0"));
    assert(runtime_chat_send("hello all"));
    flush_outbox("Me_1");
    assert(!strcmp(requested, "POST /ryunet/chat?name=Me_1&public=1") && !strcmp(posted, "hello all"));
    char said[] = "ok 1\n1\tGuest_1\thi\n";
    apply_reply(said);
    assert(view(out, NULL) == 1);
    /* out of the room: the panel goes, with what was said there */
    reply_status = 403;
    poll_room("Me_1");
    assert(!runtime_chat_available() && view(out, NULL) == 0 && !have_since);
    reply_status = 200;
    /* its option off: not even asked */
    poll_room("Me_1");
    assert(runtime_chat_available());
    runtime_chat_set_public(0);
    assert(!runtime_chat_available());
    char where[24] = "";
    assert(!in_room(where));
    runtime_chat_set_public(1);
    remove(path);
    puts("chat: ok");
    return 0;
}
