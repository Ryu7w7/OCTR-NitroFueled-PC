#define _POSIX_C_SOURCE 200809L
#include "runtime.h"
#include "runtime_hall_of_fame.h"
#include "runtime_http.h"
#include "runtime_json.h"
#include "runtime_np_online.h"
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <limits.h>
#include <errno.h>

struct HofRow { char name[17], tier[40]; int rank, mmr; };
struct HofBoard {
    struct HofRow rows[100], player;
    int count, current, *seasons, seasons_count;
    int player_status; /* 0: legacy response, 1: listed, -1: outside requested range. */
};
static pthread_mutex_t hof_lock = PTHREAD_MUTEX_INITIALIZER;
static struct HofBoard board;
static int active, opening, announced, running, mode, population, season, first_row, ready, failed;
static unsigned generation, loaded_generation;
static char local_name[17];

static int number(const char *json, const char *key, int *out) {
    const char *p = runtime_json_field(json, key);
    if (!p || !isdigit((unsigned char)*p)) return 0;
    char *end; errno = 0;
    long n = strtol(p, &end, 10);
    while (isspace((unsigned char)*end)) ++end;
    if (errno || n > INT_MAX || (*end != ',' && *end != '}')) return 0;
    *out = (int)n;
    return 1;
}

static int nickname(const char *json, char out[17]) {
    const char *p = runtime_json_field(json, "name");
    if (!p || *p++ != '"') return 0;
    size_t n = 0;
    while (*p && *p != '"') {
        if (n == 16 || (!isalnum((unsigned char)*p) && *p != '_' && *p != '-')) return 0;
        out[n++] = *p++;
    }
    out[n] = 0;
    return *p == '"' && n >= 3;
}

/* Copy one object from a bounded server array; nested objects and quoted braces are skipped. */
static char *object(const char **cursor) {
    const char *p = *cursor;
    while (isspace((unsigned char)*p) || *p == ',') ++p;
    if (*p != '{') return NULL;
    const char *start = p;
    int depth = 0, quoted = 0;
    for (; *p; ++p) {
        if (quoted) {
            if (*p == '\\') { if (!*++p) return NULL; }
            else if (*p == '"') quoted = 0;
        } else if (*p == '"') quoted = 1;
        else if (*p == '{') ++depth;
        else if (*p == '}' && --depth == 0) {
            *cursor = p + 1;
            return strndup(start, (size_t)(p + 1 - start));
        }
    }
    return NULL;
}

static int parse_row(const char *json, struct HofRow *row) {
    const char *tier = runtime_json_field(json, "tier");
    if (!tier || *tier++ != '"') return 0;
    size_t n = strcspn(tier, "\"");
    if (!n || n >= sizeof row->tier || tier[n] != '"') return 0;
    for (size_t i = 0; i < n; ++i)
        if (!isalpha((unsigned char)tier[i]) && tier[i] != ' ') return 0;
    memcpy(row->tier, tier, n); row->tier[n] = 0;
    return nickname(json, row->name) && number(json, "rank", &row->rank) && row->rank > 0 &&
        number(json, "mmr", &row->mmr);
}
static int ascending(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}
static int rank_order(const void *a, const void *b) {
    const struct HofRow *x = a, *y = b;
    return (x->rank > y->rank) - (x->rank < y->rank);
}
static int parse_board(const char *json, struct HofBoard *out) {
    if (!number(json, "current_season", &out->current) || out->current < 1) return 0;
    const char *p = runtime_json_field(json, "players");
    if (!p || *p++ != '[') return 0;
    char *item;
    while ((item = object(&p))) {
        int ok = out->count < 100 && parse_row(item, &out->rows[out->count]);
        free(item);
        if (!ok) return 0;
        ++out->count;
    }
    while (isspace((unsigned char)*p)) ++p;
    if (*p != ']') return 0;
    qsort(out->rows, (size_t)out->count, sizeof *out->rows, rank_order);
    p = runtime_json_field(json, "seasons");
    if (!p || *p++ != '[') return 0;
    while ((item = object(&p))) {
        int id = 0, ok = number(item, "id", &id) && id > 0;
        free(item);
        if (!ok) return 0;
        int *ids = realloc(out->seasons, (size_t)(out->seasons_count + 1) * sizeof *ids);
        if (!ids) return 0;
        out->seasons = ids;
        out->seasons[out->seasons_count++] = id;
    }
    while (isspace((unsigned char)*p)) ++p;
    if (*p != ']' || !out->seasons_count) return 0;
    qsort(out->seasons, (size_t)out->seasons_count, sizeof *out->seasons, ascending);
    for (int i = 1; i < out->seasons_count; ++i)
        if (out->seasons[i] == out->seasons[i - 1]) return 0;
    if (out->seasons[out->seasons_count - 1] != out->current) return 0;
    p = runtime_json_field(json, "player");
    if (p && *p == '{') {
        item = object(&p);
        int ok = item && parse_row(item, &out->player);
        const char *listed = item ? runtime_json_field(item, "in_leaderboard") : NULL;
        if (listed) {
            if (!strncmp(listed, "true", 4)) { out->player_status = 1; listed += 4; }
            else if (!strncmp(listed, "false", 5)) { out->player_status = -1; listed += 5; }
            else ok = 0;
            while (isspace((unsigned char)*listed)) ++listed;
            if (*listed != ',' && *listed != '}') ok = 0;
        }
        free(item);
        if (!ok) return 0;
    }
    return 1;
}

static void include_self(struct HofBoard *b) {
    if (b->player_status) return; /* The new API already ranks the Friends population. */
    int found = 0;
    for (int i = 0; i < b->count; ++i)
        if (!strcasecmp(b->rows[i].name, b->player.name)) found = 1;
    if (b->player.rank && !found) {
        if (b->count < 100) b->rows[b->count++] = b->player;
        else if (b->player.rank < b->rows[99].rank) b->rows[99] = b->player;
    }
    qsort(b->rows, (size_t)b->count, sizeof *b->rows, rank_order);
    for (int i = 0; i < b->count; ++i) {
        b->rows[i].rank = i + 1;
        if (b->player.rank && !strcasecmp(b->rows[i].name, b->player.name))
            b->player.rank = i + 1;
    }
}

static void *fetch_board(void *unused) {
    (void)unused;
    pthread_mutex_lock(&hof_lock);
    unsigned request = generation;
    int selected_mode = mode, selected_season = season, selected_population = population;
    char me[sizeof local_name];
    memcpy(me, local_name, sizeof me);
    pthread_mutex_unlock(&hof_lock);
    char target[160], *reply = NULL; size_t size = 0;
    snprintf(target, sizeof target, "/ryunet/ranked-leaderboard?mode=%d&count=100&season=%d&name=%s&population=%s",
             selected_mode, selected_season, me, selected_population ? "friends" : "everyone");
    struct HofBoard fetched = {0};
    int ok = runtime_ryunet_transfer("GET", target, NULL, 0, &reply, &size, 131072) == 200 &&
             reply && parse_board(reply, &fetched);
    if (ok && selected_population) {
        const char *value = runtime_json_field(reply, "population");
        /* An older server must not silently show Everyone under the Friends label. */
        ok = value && !strncmp(value, "\"friends\"", 9);
        if (ok) include_self(&fetched);
    }
    free(reply);
    pthread_mutex_lock(&hof_lock);
    if (active && request == generation) {
        ready = ok; failed = !ok; loaded_generation = request;
        if (ok) {
            free(board.seasons); board = fetched; fetched.seasons = NULL;
            if (!season) season = board.current;
            first_row = 0;
        }
    }
    running = 0;
    pthread_mutex_unlock(&hof_lock);
    free(fetched.seasons);
    return NULL;
}

static int input_activated;

void runtime_hof_open(void) {
    pthread_mutex_lock(&hof_lock);
    active = opening = 1; announced = input_activated = 0; mode = population = season = first_row = ready = failed = 0;
    ++generation;
    const char *name = runtime_np_get_online_name();
    snprintf(local_name, sizeof local_name, "%s", name ? name : "");
    pthread_mutex_unlock(&hof_lock);
}
void runtime_hof_close(void) {
    pthread_mutex_lock(&hof_lock); active = opening = 0; ++generation; pthread_mutex_unlock(&hof_lock);
}
int runtime_hof_active(void) {
    pthread_mutex_lock(&hof_lock); int yes = active; pthread_mutex_unlock(&hof_lock); return yes;
}
int runtime_hof_take_open(void) {
    pthread_mutex_lock(&hof_lock);
    int pending = active && opening;
    opening = 0;
    pthread_mutex_unlock(&hof_lock);
    return pending;
}

const char *runtime_hof_project(const char *name) {
    if (!name) return name;
    pthread_mutex_lock(&hof_lock);
    if (active && !strcmp(name, "Octane_Highscore_Select")) {
        active = opening = 0;
        ++generation;
        name = "Octane_Lobby_Main";
    }
    pthread_mutex_unlock(&hof_lock);
    return name;
}

void runtime_hof_season_changed(int current) {
    pthread_mutex_lock(&hof_lock);
    if (active && board.current != current) {
        if (!season || season == board.current) season = current;
        ++generation; ready = failed = first_row = 0;
    }
    pthread_mutex_unlock(&hof_lock);
}

static void *ptr(void *p, size_t offset) { return *(void **)((char *)p + offset); }
static void walk(void *node, int row, int control, int depth) {
    if (!node || depth > 16) return;
    const char *tag = ptr(node, 0x28);
    char text[64];
    if (tag) {
        if (sscanf(tag, "OCTR_HOF_ROW_%d", &row) == 1) {
            runtime_online_mode_show_placeable(node, ready && row + first_row < board.count);
            if (!ready || row + first_row >= board.count) return;
        }
        if (!strcmp(tag, "OCTR_HOF_SELF")) {
            int visible = ready && board.player.rank > 0 && board.player_status >= 0;
            runtime_online_mode_show_placeable(node, visible);
            if (!visible) return;
            row = -2;
        }
        if (!strcmp(tag, "OCTR_HOF_LOADING")) {
            runtime_online_mode_show_placeable(node, !ready && !failed); return;
        }
        if (!strcmp(tag, "OCTR_HOF_MODE")) control = 1;
        if (!strcmp(tag, "OCTR_HOF_POPULATION")) control = 2;
        if (!strcmp(tag, "OCTR_HOF_SEASON")) control = 3;
        const char *value = NULL;
        if (!strcmp(tag, "OCTR_HOF_TRACK_CONTAINER") || !strcmp(tag, "OCTR_HOF_TRACK") ||
            !strcmp(tag, "OCTR_HOF_PORTRAIT") || !strcmp(tag, "OCTR_HOF_STYLE")) {
            runtime_online_mode_show_placeable(node, 0); return;
        }
        if (!strcmp(tag, "AreaHeader")) value = "SEASON";
        if (!strcmp(tag, "ModeHeader")) value = "GAME MODE";
        if (!strcmp(tag, "PopulationHeader")) value = "POPULATION";
        if (!strcmp(tag, "[NameText]") && control)
            value = control == 1 ? "GAME MODE" : control == 2 ? "POPULATION" : "SEASON";
        if (!strcmp(tag, "[ItemText]") && control) {
            if (control == 1) value = mode ? "ITEMLESS" : "NORMAL";
            else if (control == 2) value = population ? "FRIENDS" : "EVERYONE";
            else { snprintf(text, sizeof text, "SEASON %d", season); value = season ? text : "LOADING..."; }
        }
        if (!strcmp(tag, "[EmptyLeaderboard]")) {
            value = failed ? "LEADERBOARD UNAVAILABLE" : ready ? "No Data" : "LOADING...";
            runtime_online_mode_show_placeable(node, !ready || !board.count);
            if (!ready || !board.count) {
                const float alpha = 1.0f;
                memcpy((char *)node + 0xe0, &alpha, sizeof alpha);
            }
        }
        if (row == -2 || (row >= 0 && row + first_row < board.count)) {
            const struct HofRow *r = row == -2 ? &board.player : &board.rows[row + first_row];
            if (!strcmp(tag, "OCTR_HOF_NAME")) value = r->name;
            if (!strcmp(tag, "OCTR_HOF_RANK")) { snprintf(text, sizeof text, "%d", r->rank); value = text; }
            if (!strcmp(tag, "OCTR_HOF_TIER")) value = r->tier;
            if (!strcmp(tag, "OCTR_HOF_MMR")) {
                snprintf(text,sizeof text,"%d MMR",r->mmr); value=text;
                void *parent=ptr(node,0x40), *children=parent ? ptr(parent,0x48) : NULL;
                int count=children ? *(int *)((char *)children+12) : 0;
                void **items=children ? ptr(children,0x20) : NULL;
                for (int i=0;items && i<count && i<256;++i) {
                    const char *name=items[i] ? ptr(items[i],0x28) : NULL;
                    if (!name || strcmp(name,"OCTR_HOF_TIER")) continue;
                    if (memcmp((char *)node+0xc0,(char *)items[i]+0xc0,16)) {
                        memcpy((char *)node+0xc0,(char *)items[i]+0xc0,16);
                        *(unsigned *)((char *)node+12)|=0x1000;
                    }
                    break;
                }
            }
            int icon_mode, icon_tier;
            if (sscanf(tag, "OCTR_HOF_ICON_%d_%d", &icon_mode, &icon_tier) == 2)
                runtime_online_mode_show_placeable(node, icon_mode == mode &&
                    !strcmp(r->tier, runtime_ranked_tier_name(icon_mode, icon_tier)));
        }
        if (value) runtime_online_mode_set_text(node, value);
    }
    void *list = ptr(node, 0x48);
    int count = list ? *(int *)((char *)list + 12) : 0;
    void **items = list ? ptr(list, 0x20) : NULL;
    for (int i = 0; items && i < count && i < 256; ++i) walk(items[i], row, control, depth + 1);
    walk(runtime_online_mode_content(node), row, control, depth + 1);
}

void runtime_hof_update(void *root, void *project) {
    if (!root) return;
    const char *tag = ptr(root, 0x28);
    if (!tag || strcmp(tag, "OCTR_HOF_ROOT")) return;
    (void)runtime_ranked_status(0);
    pthread_mutex_lock(&hof_lock);
    if (!active) { pthread_mutex_unlock(&hof_lock); return; }
    if (!running && loaded_generation != generation) {
        pthread_t worker;
        running = 1;
        if (pthread_create(&worker, NULL, fetch_board, NULL)) {
            running = 0; failed = 1; loaded_generation = generation;
        } else pthread_detach(worker);
    }
    walk(root, -1, 0, 0);
    int notify = !announced;
    announced = 1;
    pthread_mutex_unlock(&hof_lock);
    if (notify) runtime_hof_native_ready(project);
    /* The native entry transition deactivates input after Ready. Restore it once. */
    else if (!input_activated) input_activated = runtime_online_mode_hof_activate(project);
}

int runtime_hof_input(void *placeable, unsigned signal) {
    const char *tag = placeable ? ptr(placeable, 0x28) : NULL;
    if (!tag || strncmp(tag, "OCTR_HOF_", 9)) return 0;
    pthread_mutex_lock(&hof_lock);
    if (!active) { pthread_mutex_unlock(&hof_lock); return 0; }
    if (signal == 10) {
        pthread_mutex_unlock(&hof_lock);
        runtime_hof_focus_list(placeable);
        return 1;
    }
    int handled = 0, changed = 0;
    unsigned direction = signal & 3;
    int row = -1;
    if (signal < 8 && sscanf(tag, "OCTR_HOF_ROW_%d", &row) == 1 && direction < 2) {
        int visible = board.count - first_row;
        if (visible > 8) visible = 8;
        if (direction == 0 && row == 0) {
            if (first_row > 0) --first_row;
            handled = 1;
        } else if (direction == 1 && row + 1 >= visible) {
            if (ready && first_row + 8 < board.count) ++first_row;
            handled = 1;
        }
    } else if (signal < 8 && direction >= 2) {
        int delta = direction == 2 ? -1 : 1;
        if (!strcmp(tag, "OCTR_HOF_MODE")) {
            int next = delta > 0;
            changed = mode != next; mode = next; handled = 1;
        } else if (!strcmp(tag, "OCTR_HOF_POPULATION")) {
            int next = delta > 0;
            changed = population != next; population = next; handled = 1;
        } else if (!strcmp(tag, "OCTR_HOF_SEASON")) {
            for (int i = 0; i < board.seasons_count; ++i) if (board.seasons[i] == season) {
                int next = i + delta;
                if (next >= 0 && next < board.seasons_count) { season = board.seasons[next]; changed = 1; }
                break;
            }
            handled = 1;
        }
    } else if (signal < 8 && !strcmp(tag, "OCTR_HOF_LIST")) {
        int next = first_row + (direction == 0 ? -1 : 1);
        /* At the first row, native navigation must reach the selectors again. */
        handled = next >= 0;
        if (ready && next >= 0 && next + 8 <= board.count) first_row = next;
    }
    if (signal < 8 && direction == 1 && !strcmp(tag, "OCTR_HOF_SEASON")) handled = 1;
    if (signal == 8) { changed = handled = 1; }
    if (changed) { ++generation; ready = failed = first_row = 0; }
    pthread_mutex_unlock(&hof_lock);
    if (changed && signal < 8 && direction >= 2)
        runtime_online_mode_hof_cycle(placeable, direction == 2 ? -1 : 1);
    return handled;
}
