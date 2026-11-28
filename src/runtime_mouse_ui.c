#include "runtime.h"
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <SDL3/SDL.h>

/* Observe the native GUI poll without consuming any signals. The first call confirms that
 * the matched slot is used by a live GUI object before mouse navigation is added. */
typedef float (ABI *GuiSignal)(void *, int, int);
static GuiSignal original_signal;
static atomic_flag reported = ATOMIC_FLAG_INIT;
static atomic_int enabled;
static int trace;

int runtime_mouse_ui_enabled(void) {
    return atomic_load_explicit(&enabled, memory_order_relaxed);
}

/* Normalized SDL coordinates, published together so the GUI poll never mixes two events. */
static _Atomic uint64_t pointer_position = UINT64_MAX;
static _Atomic uint64_t click_position = UINT64_MAX;
static _Atomic uint64_t scroll_position = UINT64_MAX;
/* One value makes the command and its expiry coherent for the pad reader. */
static _Atomic uint64_t pad_pulse;
static _Atomic uint64_t menu_cursor_until;

int runtime_mouse_ui_cursor_visible(void) {
    const uint64_t deadline = atomic_load_explicit(&menu_cursor_until, memory_order_relaxed);
    return deadline && SDL_GetTicks() < deadline;
}

enum MouseCommand runtime_mouse_ui_command(void) {
    if (atomic_load_explicit(&pointer_position, memory_order_relaxed) == UINT64_MAX)
        return MOUSE_UI_NONE;
    const uint64_t pulse = atomic_load_explicit(&pad_pulse, memory_order_relaxed);
    return pulse && SDL_GetTicks() < (pulse >> 8) ? (enum MouseCommand)(pulse & 0xff) : MOUSE_UI_NONE;
}

static uint64_t encode_pointer(float x, float y) {
    if (x >= 0 && x < 1 && y >= 0 && y < 1)
        return ((uint64_t)(x * 65536.0f) << 16) | (uint64_t)(y * 65536.0f);
    return UINT64_MAX;
}

void runtime_mouse_ui_scroll(float x, float y, int previous) {
    const uint64_t position = encode_pointer(x, y);
    if (position == UINT64_MAX) return;
    atomic_store_explicit(&pointer_position, position, memory_order_relaxed);
    atomic_store_explicit(&scroll_position, position | ((uint64_t)!!previous << 32),
                          memory_order_relaxed);
}

void runtime_mouse_ui_pointer(float x, float y, int click) {
    const uint64_t position = encode_pointer(x, y);
    atomic_store_explicit(&pointer_position, position, memory_order_relaxed);
    if (click || position == UINT64_MAX)
        atomic_store_explicit(&click_position, position, memory_order_relaxed);
    if (position == UINT64_MAX) {
        atomic_store_explicit(&scroll_position, UINT64_MAX, memory_order_relaxed);
        atomic_store_explicit(&pad_pulse, 0, memory_order_relaxed);
        atomic_store_explicit(&menu_cursor_until, 0, memory_order_relaxed);
    }
}

static int read_guest(uintptr_t object, size_t offset, void *value, size_t size) {
    if (!object || object >= (UINT64_C(1) << 40) ||
        !runtime_memory_is_mapped(object + offset, size)) return 0;
    memcpy(value, (const void *)(object + offset), size);
    return 1;
}

static int read_list(uintptr_t list, uintptr_t *items, int limit) {
    int count;
    uintptr_t data;
    if (!read_guest(list, 0xc, &count, sizeof count) ||
        !read_guest(list, 0x20, &data, sizeof data) || count <= 0) return 0;
    if (count > limit) count = limit;
    return read_guest(data, 0, items, (size_t)count * sizeof(*items)) ? count : 0;
}

typedef struct {
    float left, top, right, bottom;
} MenuBounds;

static int menu_bounds(uintptr_t item, MenuBounds *bounds) {
    uint32_t flags;
    uintptr_t state;
    float world_and_box[21];
    if (!read_guest(item, 0xc, &flags, sizeof flags) || (flags & 0x2000) ||
        !read_guest(item, 0x100, &state, sizeof state) ||
        !read_guest(state, 0, world_and_box, sizeof world_and_box)) return 0;
    const float *s = world_and_box;
    const float x0 = s[12] + s[16] * s[0], x1 = s[12] + s[19] * s[0];
    const float y0 = s[13] + s[17] * s[5], y1 = s[13] + s[20] * s[5];
    if (!isfinite(x0) || !isfinite(x1) || !isfinite(y0) || !isfinite(y1)) return 0;
    *bounds = (MenuBounds){fminf(x0, x1), fminf(y0, y1), fmaxf(x0, x1), fmaxf(y0, y1)};
    return bounds->right - bounds->left > 1 && bounds->bottom - bounds->top > 1;
}

/* Field offsets follow FrankHUN88453's menu implementation. Pick the highest-priority
 * active player-one project and only consider visible siblings of the current focus. */
static int contains_pointer(const MenuBounds *bounds, float x, float y) {
    return x >= bounds->left && x < bounds->right && y >= bounds->top && y < bounds->bottom;
}

static uintptr_t guest_pointer(uintptr_t object, size_t offset) {
    uintptr_t result = 0;
    read_guest(object, offset, &result, sizeof result);
    return result;
}

/* These placeables keep the displayed button legend inside a resolved composition.
 * The Cross glyph identifies confirm independently of the language of its label. */
static uintptr_t legend_control_at(uintptr_t node, float x, float y, int depth, int *budget,
                                  int in_legend, int paging) {
    if (!node || depth > 10 || *budget <= 0) return 0;
    --*budget;
    uint32_t flags;
    if (!read_guest(node, 0xc, &flags, sizeof flags) || (flags & 0x2000)) return 0;
    const uintptr_t text = guest_pointer(node, 0x30);
    static const char legend_name[] = "ButtonLegendHorizontal";
    char name[sizeof legend_name];
    if (!in_legend && read_guest(text, 0, name, sizeof name) &&
        !memcmp(name, legend_name, sizeof name)) in_legend = 1;
    char labels[160];
    if (in_legend && read_guest(text, 0, labels, sizeof labels) && memchr(labels, 0, sizeof labels)) {
        static const char cross[] = "\xee\x80\x80", stick[] = "\xee\x80\x89";
        static const char circle[] = "\xee\x80\x81", r1[] = "\xee\x80\x86";
        static const char l1[] = "\xee\x80\x84", l2[] = "\xee\x80\x87";
        if (paging && strstr(labels, circle)) {
            const int ordinary = !memcmp(labels, stick, 3) && strstr(labels + 3, r1);
            const int last = paging < 0 && !memcmp(labels, cross, 3) &&
                             strstr(labels + 3, stick) && strstr(labels + 3, l1) && strstr(labels + 3, l2);
            if (ordinary || last) return node;
        } else if (!paging && !memcmp(labels, cross, 3) && !strstr(labels + 3, "\xee\x80")) {
            MenuBounds bounds;
            if (menu_bounds(node, &bounds) && contains_pointer(&bounds, x, y)) return node;
        }
    }
    const uintptr_t handle = guest_pointer(node, 0x38);
    uint32_t handle_flags;
    if (read_guest(handle, 0, &handle_flags, sizeof handle_flags) &&
        (handle_flags & 0x70000000) == 0x40000000) {
        const uintptr_t asset = guest_pointer(handle, 0x28);
        const uintptr_t root = guest_pointer(asset, 0x20);
        if (root && root != node) {
            const uintptr_t target = legend_control_at(root, x, y, depth + 1, budget, in_legend, paging);
            if (target) return target;
        }
    }

    uintptr_t children[20];
    const int count = read_list(guest_pointer(node, 0x48), children, 20);
    for (int i = 0; i < count; ++i) {
        const uintptr_t target = legend_control_at(children[i], x, y, depth + 1, budget, in_legend, paging);
        if (target) return target;
    }
    return 0;
}
static uintptr_t mouse_menu_target(uintptr_t input, float x, float y, enum MouseCommand *direction, int *active, int scroll) {
    *direction = MOUSE_UI_NONE;
    *active = 0;
    uintptr_t context, projects, list[64];
    float viewport[2];
    if (!read_guest(input, 0x80, &context, sizeof context) ||
        !read_guest(context, 0x28, viewport, sizeof viewport) ||
        !read_guest(context, 0x58, &projects, sizeof projects)) return 0;
    if (!(viewport[0] > 0 && viewport[0] <= 16384 && viewport[1] > 0 && viewport[1] <= 16384))
        return 0;
    x *= viewport[0];
    y *= viewport[1];

    uintptr_t focus = 0, active_project = 0;
    int best_priority = -1;
    const int count = read_list(projects, list, 64);
    for (int i = 0; i < count; ++i) {
        uint8_t flags;
        int priority, controllers, disabled;
        uintptr_t selected;
        if (!read_guest(list[i], 0x34, &flags, sizeof flags) || (flags & 0x31) != 0x21 ||
            !read_guest(list[i], 0x44, &controllers, sizeof controllers) || !(controllers & 1) ||
            !read_guest(list[i], 0x48, &disabled, sizeof disabled) || disabled > 0 ||
            !read_guest(list[i], 0x20, &priority, sizeof priority) || priority < best_priority ||
            !read_guest(list[i], 0x50, &selected, sizeof selected)) continue;
        // Later projects are above earlier projects at the same priority.
        active_project = list[i];
        focus = selected;
        best_priority = priority;
    }

    if (scroll) {
        int budget = 160;
        const uintptr_t target = legend_control_at(guest_pointer(active_project, 0x10), x, y, 0,
                                                   &budget, 0, scroll);
        if (target) {
            *active = 1;
            *direction = scroll < 0 ? MOUSE_UI_PREVIOUS_PAGE : MOUSE_UI_NEXT_PAGE;
        }
        return target;
    }

    uintptr_t parent, children, siblings[32];
    MenuBounds selected;
    if (!menu_bounds(focus, &selected)) {
        if (focus || !active_project) return 0;
        int budget = 160;
        const uintptr_t target = legend_control_at(guest_pointer(active_project, 0x10), x, y, 0, &budget, 0, 0);
        if (target) {
            *active = 1;
            *direction = MOUSE_UI_CONFIRM;
        }
        return target;
    }
    *active = 1;
    const int on_focus = contains_pointer(&selected, x, y);
    uintptr_t target = on_focus ? focus : 0;
    if (on_focus) *direction = MOUSE_UI_CONFIRM;
    if (!read_guest(focus, 0x40, &parent, sizeof parent) ||
        !read_guest(parent, 0x48, &children, sizeof children)) return target;
    const float width = selected.right - selected.left, height = selected.bottom - selected.top;
    const int candidates = read_list(children, siblings, 32);

    float nearest = on_focus ? x - selected.left + y - selected.top : INFINITY;
    for (int i = 0; i < candidates; ++i) {
        MenuBounds item;
        if (!menu_bounds(siblings[i], &item) ||
            fabsf(item.right - item.left - width) > width * .3f ||
            fabsf(item.bottom - item.top - height) > height * .3f ||
            !contains_pointer(&item, x, y)) continue;
        const float distance = x - item.left + y - item.top;
        if (distance < nearest) {
            nearest = distance;
            target = siblings[i];
            const float dx = (item.left + item.right - selected.left - selected.right) * .5f;
            const float dy = (item.top + item.bottom - selected.top - selected.bottom) * .5f;
            if (target == focus)
                *direction = MOUSE_UI_CONFIRM;
            else if (fabsf(dy) >= fabsf(dx) * .5f)
                *direction = dy > 0 ? MOUSE_UI_DOWN : MOUSE_UI_UP;
            else
                *direction = dx > 0 ? MOUSE_UI_RIGHT : MOUSE_UI_LEFT;
        }
    }
    return target;
}
/* Re-evaluate after each step because the menu owns focus and may change its layout. */
static void update_navigation(uintptr_t input) {
    static _Thread_local uint64_t last_position = UINT64_MAX, requested = UINT64_MAX;
    static _Thread_local uint64_t next_step;
    static _Thread_local uintptr_t initial_target;
    static _Thread_local int steps, confirm, scroll;
    const uint64_t pointer = atomic_load_explicit(&pointer_position, memory_order_relaxed);
    const uint64_t click = atomic_exchange_explicit(&click_position, UINT64_MAX, memory_order_relaxed);
    const uint64_t wheel = atomic_exchange_explicit(&scroll_position, UINT64_MAX, memory_order_relaxed);
    if (pointer != last_position || click != UINT64_MAX || wheel != UINT64_MAX) {
        last_position = pointer;
        requested = click == UINT64_MAX ? pointer : click;
        confirm = click != UINT64_MAX;
        scroll = 0;
        if (wheel != UINT64_MAX && !confirm) {
            requested = wheel & UINT32_MAX;
            scroll = (wheel >> 32) ? -1 : 1;
        }
        initial_target = 0;
        steps = 0;
    }
    if (requested == UINT64_MAX) return;
    const uint64_t now = SDL_GetTicks();
    if (now < next_step) return;

    enum MouseCommand direction;
    int active;
    const uintptr_t target = mouse_menu_target(input, (float)(requested >> 16) / 65536.0f,
        (float)(requested & 0xffff) / 65536.0f, &direction, &active, scroll);
    atomic_store_explicit(&menu_cursor_until, active ? now + 3000 : 0, memory_order_relaxed);
    if (!target || (initial_target && target != initial_target) || steps >= 12) {
        requested = UINT64_MAX;
        return;
    }
    initial_target = target;
    if (direction != MOUSE_UI_CONFIRM || confirm) {
        atomic_store_explicit(&pad_pulse, ((now + 65) << 8) | direction, memory_order_relaxed);
        next_step = now + 120; /* release before the next synthetic press */
        ++steps;
    }
    if (direction == MOUSE_UI_CONFIRM || scroll) requested = UINT64_MAX;
}

static float ABI trace_signal(void *self, int signal, int controllers) {
    if (trace && !atomic_flag_test_and_set_explicit(&reported, memory_order_relaxed))
        puts("Mouse UI: native GUI signal callback reached");
    if (controllers & 1) update_navigation((uintptr_t)self);
    return original_signal(self, signal, controllers);
}
int runtime_mouse_ui_install(void *slot, uintptr_t expected_callback) {
    uintptr_t current;
    memcpy(&current, slot, sizeof current);
    if (current != expected_callback) return 0;
    original_signal = (GuiSignal)current;
    const uintptr_t replacement = (uintptr_t)&trace_signal;
    memcpy(slot, &replacement, sizeof replacement);
    const char *diagnostics = getenv("BB_MOUSE_UI_TRACE");
    trace = diagnostics && !strcmp(diagnostics, "1");
    atomic_store_explicit(&enabled, 1, memory_order_relaxed);
    puts("Mouse UI: enabled native menu input");
    return 1;
}
