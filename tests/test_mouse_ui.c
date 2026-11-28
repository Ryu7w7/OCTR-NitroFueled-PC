/* Synthetic guest GUI objects. No game data or window required. */
#define _GNU_SOURCE
#include <assert.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <SDL3/SDL.h>
static uint64_t ticks = 1000;
#define SDL_GetTicks() ticks
#include "../src/runtime_mouse_ui.c"

static uintptr_t allocation;

static void allocate_guest(void) {
#ifdef _WIN32
    allocation = (uintptr_t)VirtualAlloc((void *)0x40000000, 4096,
                                          MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(allocation);
#else
    void *memory = mmap((void *)0x40000000, 4096, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(memory != MAP_FAILED && (uintptr_t)memory < (UINT64_C(1) << 40));
    allocation = (uintptr_t)memory;
#endif
}
int runtime_memory_is_mapped(uintptr_t address, uint64_t size) {
    return address >= allocation && size <= 4096 && address - allocation <= 4096 - size;
}
static void integer(uintptr_t object, size_t offset, uint32_t value) {
    memcpy((void *)(object + offset), &value, sizeof value);
}
static void pointer(uintptr_t object, size_t offset, uintptr_t value) {
    memcpy((void *)(object + offset), &value, sizeof value);
}
static void place(uintptr_t obj, uintptr_t state, float x, float y) {
    float matrix[21] = {[0]=1, [5]=1, [12]=x, [13]=y, [19]=200, [20]=60};
    pointer(obj, 0x100, state);
    memcpy((void *)state, matrix, sizeof matrix);
}
static float ABI fake_signal(void *self, int signal, int controllers) {
    assert(self != NULL);
    return (float)(signal + controllers);
}
int main(void) {
    uintptr_t slot = (uintptr_t)&fake_signal;
    assert(!runtime_mouse_ui_enabled());
    assert(!runtime_mouse_ui_install(&slot, slot + 1));
    assert(!runtime_mouse_ui_enabled());
    assert(runtime_mouse_ui_install(&slot, (uintptr_t)&fake_signal));
    assert(runtime_mouse_ui_enabled());
    assert(((GuiSignal)slot)((void *)1, 2, 0) == 2);
    allocate_guest();
    uintptr_t input=allocation, context=input+0x100, projects=input+0x200, project=input+0x300;
    uintptr_t first=input+0x400, second=input+0x600, parent=input+0x800, children=input+0x900;
    pointer(input, 0x80, context);
    float viewport[] = {1000, 500};
    memcpy((void *)(context+0x28), viewport, sizeof viewport);
    pointer(context, 0x58, projects);
    integer(projects, 0xc, 1);
    pointer(projects, 0x20, projects+0x30);
    pointer(projects, 0x30, project);
    integer(project, 0x34, 0x21);
    integer(project, 0x44, 1);
    pointer(project, 0x50, first);
    pointer(first, 0x40, parent);
    pointer(parent, 0x48, children);
    integer(children, 0xc, 2);
    pointer(children, 0x20, children+0x30);
    pointer(children, 0x30, first);
    pointer(children, 0x38, second);
    place(first, input+0xa00, 100, 100);
    place(second, input+0xb00, 100, 150);
    enum MouseCommand direction;
    int active;
    assert(mouse_menu_target(input, .15f, .22f, &direction, &active, 0) == first);
    assert(direction == MOUSE_UI_CONFIRM);
    runtime_mouse_ui_pointer(.15f, .31f, 1);
    update_navigation(input);
    assert(runtime_mouse_ui_command() == MOUSE_UI_DOWN);
    assert(runtime_mouse_ui_cursor_visible());
    ticks += 70;
    assert(runtime_mouse_ui_command() == MOUSE_UI_NONE);
    pointer(project, 0x50, second);
    pointer(second, 0x40, parent);
    ticks += 50;
    update_navigation(input);
    assert(runtime_mouse_ui_command() == MOUSE_UI_CONFIRM);
    ticks += 130;
    update_navigation(input);
    assert(runtime_mouse_ui_command() == MOUSE_UI_NONE);
    pointer(project, 0x50, first);
    assert(mouse_menu_target(input, .15f, .31f, &direction, &active, 0) == second);
    assert(mouse_menu_target(input, .50f, .31f, &direction, &active, 0) == 0);
    integer(second, 0xc, 0x2000);
    assert(mouse_menu_target(input, .15f, .31f, &direction, &active, 0) == first);
    integer(project, 0x48, 1);
    assert(mouse_menu_target(input, .15f, .22f, &direction, &active, 0) == 0);
    integer(project, 0x48, 0);
    const uintptr_t modal = input + 0xc00;
    integer(projects, 0xc, 2);
    pointer(projects, 0x38, modal);
    integer(modal, 0x34, 0x21);
    integer(modal, 0x44, 1);
    integer(modal, 0x20, 0);
    assert(mouse_menu_target(input, .15f, .22f, &direction, &active, 0) == 0);
    integer(projects, 0xc, 1);
    pointer(first, 0x40, 0);
    assert(mouse_menu_target(input, .15f, .22f, &direction, &active, 0) == first);
    assert(direction == MOUSE_UI_CONFIRM);
    integer(projects, 0xc, 2);
    const uintptr_t legend = input + 0xb80, hint = input + 0xd00, hints = input + 0xe00;
    pointer(modal, 0x10, legend);
    pointer(legend, 0x30, input + 0xe80);
    memcpy((void *)(input + 0xe80), "ButtonLegendHorizontal", sizeof "ButtonLegendHorizontal");
    pointer(legend, 0x48, hints);
    integer(hints, 0xc, 1);
    pointer(hints, 0x20, hints + 0x30);
    pointer(hints, 0x30, hint);
    pointer(hint, 0x30, input + 0xf00);
    const char *paging = "\xee\x80\x89 Previous/Next Page    \xee\x80\x86 Next Page    \xee\x80\x81 Decline";
    memcpy((void *)(input + 0xf00), paging, strlen(paging) + 1);
    assert(mouse_menu_target(input, .50f, .50f, &direction, &active, 1) == hint);
    assert(direction == MOUSE_UI_NEXT_PAGE);
    assert(mouse_menu_target(input, .50f, .50f, &direction, &active, -1) == hint);
    assert(direction == MOUSE_UI_PREVIOUS_PAGE);
    const char *last = "\xee\x80\x80 Accept    \xee\x80\x89 Previous/Next Page    \xee\x80\x84 Previous Page    \xee\x80\x87 First Page    \xee\x80\x81 Decline";
    memcpy((void *)(input + 0xf00), last, strlen(last) + 1);
    assert(mouse_menu_target(input, .50f, .50f, &direction, &active, 1) == 0);
    assert(mouse_menu_target(input, .50f, .50f, &direction, &active, -1) == hint);
    assert(direction == MOUSE_UI_PREVIOUS_PAGE);
    assert(mouse_menu_target(input, .50f, .50f, &direction, &active, 0) == 0);
    assert(direction == MOUSE_UI_NONE);
    place(hint, input + 0xa00, 400, 220);
    assert(mouse_menu_target(input, .50f, .50f, &direction, &active, 0) == 0);
    const char *confirm_label = "\xee\x80\x80 Continue";
    memcpy((void *)(input + 0xf00), confirm_label, strlen(confirm_label) + 1);
    assert(mouse_menu_target(input, .50f, .50f, &direction, &active, 0) == hint);
    assert(direction == MOUSE_UI_CONFIRM);
    assert(mouse_menu_target(input, .80f, .50f, &direction, &active, 0) == 0);
    memset((void *)(input + 0xf00), 'x', 160);
    memcpy((void *)(input + 0xf00), confirm_label, 3);
    assert(mouse_menu_target(input, .50f, .50f, &direction, &active, 0) == 0);
    runtime_mouse_ui_pointer(-1, -1, 0);
    assert(!runtime_mouse_ui_cursor_visible());
    assert(runtime_mouse_ui_command() == MOUSE_UI_NONE);
#ifdef _WIN32
    assert(VirtualFree((void *)allocation, 0, MEM_RELEASE));
#else
    assert(munmap((void *)allocation, 4096) == 0);
#endif
}
