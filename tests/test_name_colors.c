/* The nicknames' colors, without a server or a game. */
#include "../src/runtime_name_colors.c"
#include <assert.h>

static const char *reply_text = "ok\n";
static int reply_status = 200, asked;

int runtime_ryunet_transfer(const char *method, const char *target, const void *data, size_t size, char **reply, size_t *reply_size,
                            size_t max_reply) {
    (void)data, (void)size, (void)max_reply;
    assert(!strcmp(method, "GET") && !strcmp(target, "/ryunet/names"));
    asked++;
    *reply = strdup(reply_text);
    *reply_size = strlen(reply_text);
    return reply_status;
}

static int near(float a, float b) { return a - b < 0.001f && b - a < 0.001f; }

int main(void) {
    float rgb[3] = {-1, -1, -1};

    /* the administrator, before the server answers */
    assert(!runtime_name_color("Someone", rgb) && !runtime_name_color(NULL, rgb) && !runtime_name_color("", rgb));
    assert(rgb[0] == -1.0f);
    assert(runtime_name_color("RYU7W7", rgb));
    for (int i = 0; i < 3; ++i) assert(rgb[i] >= RAINBOW_FLOOR && rgb[i] <= 1.0f);

    /* a whole turn, slow and without a jump */
    rainbow_at(0, 12000, rgb);
    assert(near(rgb[0], 1) && near(rgb[1], RAINBOW_FLOOR) && near(rgb[2], RAINBOW_FLOOR));
    rainbow_at(4000, 12000, rgb);
    assert(near(rgb[1], 1));
    rainbow_at(8000, 12000, rgb);
    assert(near(rgb[2], 1));
    float before[3];
    rainbow_at(11999, 12000, before);
    rainbow_at(12000, 12000, rgb);
    assert(near(rgb[0], before[0]) && rgb[2] - before[2] < 0.01f);

    /* the server's list replaces it, also its speed, and what cannot be read stays out */
    reply_text = "ok\nstaff_1\t#ff8000\nRyu7w7\trainbow:30\nbad line\nno\t#ffffff\nnocolor\tred\nslow\trainbow:999\nfast\trainbow:2\n"
                 "toolongtobeanicknamehere\t#000000\ncrlf_1\t#00ff00\r\n";
    assert(fetch_names());
    assert(current->count == 3);
    assert(runtime_name_color("Staff_1", rgb) && near(rgb[0], 1) && near(rgb[1], 128 / 255.0f) && near(rgb[2], 0));
    assert(runtime_name_color("ryu7w7", rgb) && current->names[1].turn_ms == 30000);
    assert(runtime_name_color("crlf_1", rgb) && near(rgb[1], 1));
    assert(!runtime_name_color("slow", rgb) && !runtime_name_color("no", rgb));

    /* the same list again is not a new one */
    const NameList *same = current;
    assert(fetch_names() && current == same);

    /* a failure or something else keeps what there is */
    reply_status = 500;
    assert(!fetch_names() && current == same);
    reply_status = 200;
    reply_text = "<html>not this</html>";
    assert(!fetch_names() && current == same);
    reply_text = "okay\nstaff_1\t#000000\n";
    assert(!fetch_names() && current == same);

    /* an empty list empties it, the administrator's name too */
    reply_text = "ok\n";
    assert(fetch_names() && current->count == 0 && !runtime_name_color("Ryu7w7", rgb));

    /* no more than fit */
    static char many[4096];
    int used = snprintf(many, sizeof many, "ok\n");
    for (int i = 0; i < NAMES_MAX + 10; ++i) used += snprintf(many + used, sizeof many - (size_t)used, "nick_%d\t#010203\n", i);
    reply_text = many;
    assert(fetch_names() && current->count == NAMES_MAX);
    assert(asked == 7);
    puts("name colors: ok");
    return 0;
}
