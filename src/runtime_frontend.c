/* SacredFire: the title screen and the kart customization screen in ultrawide. Both are 3D sets
 * whose checkered backdrop is only as wide as a 16:9 view; the game widens the horizontal field of
 * view with the aspect (Hor+), so at 21:9 the camera sees past the backdrop's edges (sky, black).
 * On these screens the camera gets the horizontal field of view a 16:9 screen would have (the
 * same width of the set, less of its top and bottom); the menus are drawn separately and keep
 * their ultrawide layout. (More screens can be added to screens[]: BB_FRONTEND_TRACE.) Cutscenes
 * (runtime_cinematic_cutscene(), with ultrawide_cinematics on) get the same: the 16:9 shot fills
 * the width, their subtitles keep the game's layout, and the presenter no longer crops them. Their
 * camera's field of view changes from shot to shot, so the factor follows it. Races and other
 * screens keep the game's own field of view.
 *
 * The camera's field of view (degrees) is written each frame by one instruction,
 * vmulss xmm0, xmm0, [rip+57.29578] (radians to degrees) followed by the store to the render
 * camera's +0x3c. The runtime points that instruction at a float of its own (in the unused tail of
 * the image's last data page), which holds 57.29578 normally and a smaller factor on the screen.
 * Screens are recognised by the main render camera resting at their set's camera position
 * (the main render pass, CPreRenderUpdatePass with _cameraName "main", has the camera at +0x168).
 *
 * BB_FRONTEND_FOV="INSTRUCTION PASS_VTABLE SLOT" (image offsets; scripts/patches.py
 * --print-frontend) turns it on. BB_FRONTEND_TRACE=1 logs where the camera comes to rest (to add a
 * screen). Linux only (the render pass is found through /proc/self/maps). */
#define _GNU_SOURCE /* usleep */
#include "runtime.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#define GUEST_MIN UINT64_C(0x1000000000)
#define GUEST_LIMIT (UINT64_C(1) << 40)
#define DEGREES 57.29578f
/* CPreRenderUpdatePass fields (reflection) and the render camera's layout. */
#define PASS_CAMERA_NAME 0x138
#define PASS_RENDER_CAMERA 0x168
#define CAMERA_FOV 0x3c      /* horizontal field of view, degrees */
#define CAMERA_ASPECT 0x40   /* width / height */
#define CAMERA_POSITION 0xa0 /* three floats */

struct screen {
    const char *name;
    float x, y, z; /* the set's camera position */
};
static const struct screen screens[] = {
    {"title screen", 5369.079f, -145.722f, -929.984f},
    {"kart customization", -104.853f, -57.029f, -914.018f},
};
#define SCREEN_COUNT (int)(sizeof(screens) / sizeof(screens[0]))
#define CUTSCENE SCREEN_COUNT /* a cutscene plays: the screen after screens[] */

static uint64_t pass_vtable;
static volatile float *slot; /* the factor the field-of-view instruction multiplies by */
/* The title screen's camera lowered a little (its logo then clears "Press X to begin"): the store
 * of the camera's position (vmovups [rbx+0xa0],xmm3) jumps to a few instructions in a page next to
 * the image that add this vector first. Zero elsewhere. */
static volatile float *position_offset;
/* The camera matrix's rows (render camera +0x70, +0x80, +0x90; found by trying each on the title
 * screen): 0x70 forward (zoom), 0x80 sideways, 0x90 vertical, pointing down on screen. The offset is
 * -title_lower times the row: -12 along 0x90 moves the logo about 30 pixels (of 330) up, clear of
 * "Press X to begin". BB_TITLE_LOWER and BB_TITLE_AXIS override them (tuning). */
static float title_lower = -12.0f;
static int title_axis = 0x90;
static int trace;

/* The factor that turns the game's horizontal field of view (degrees) into a 16:9 screen's at
 * this aspect; 0 when it does not apply. */
static float factor_for(float fov, float aspect) {
    if (aspect <= 16.0f / 9.0f + 0.01f || fov <= 1.0f || fov >= 170.0f) return 0.0f;
    const float half = fov * 0.5f / DEGREES;
    const float target = 2.0f * atanf(tanf(half) * (16.0f / 9.0f) / aspect) * DEGREES;
    return DEGREES * target / fov;
}

/* Readable regions of the process, so a stray vtable match is never followed into unmapped
 * memory. */
#define MAX_REGIONS 4096
static unsigned long long region_start[MAX_REGIONS], region_end[MAX_REGIONS];
static int region_count;

static int readable(uint64_t address, uint64_t size) {
    for (int i = 0; i < region_count; ++i)
        if (address >= region_start[i] && address + size <= region_end[i]) return 1;
    return 0;
}

static void read_regions(void) {
    FILE *maps = fopen("/proc/self/maps", "r");
    char line[512];
    region_count = 0;
    while (maps && region_count < MAX_REGIONS && fgets(line, sizeof line, maps)) {
        char perm[8];
        if (sscanf(line, "%llx-%llx %7s", &region_start[region_count], &region_end[region_count], perm) == 3 &&
            perm[0] == 'r')
            ++region_count;
    }
    if (maps) fclose(maps);
}

/* The main render pass: created once, at start; scanned for until found. */
static uint64_t find_main_pass(void) {
    read_regions();
    for (int r = 0; r < region_count; ++r) {
        const uint64_t a = region_start[r], b = region_end[r];
        if (a < GUEST_MIN || b > GUEST_LIMIT) continue;
        for (uint64_t p = a; p + PASS_RENDER_CAMERA + 8 <= b; p += 8) {
            if (*(const uint64_t *)(uintptr_t)p != pass_vtable) continue;
            const uint64_t name = *(const uint64_t *)(uintptr_t)(p + PASS_CAMERA_NAME);
            const uint64_t camera = *(const uint64_t *)(uintptr_t)(p + PASS_RENDER_CAMERA);
            if (readable(name, 5) && !memcmp((const void *)(uintptr_t)name, "main", 5) && readable(camera, 0x100))
                return p;
        }
    }
    return 0;
}

static int at(const float *pos, const struct screen *s) {
    return fabsf(pos[0] - s->x) < 1.0f && fabsf(pos[1] - s->y) < 1.0f && fabsf(pos[2] - s->z) < 1.0f;
}

static void *frontend_thread(void *unused) {
    (void)unused;
    uint64_t pass = 0;
    while (!pass) {
        sleep(2);
        pass = find_main_pass();
    }
    printf("Frontend field of view: main render pass found\n");
    fflush(stdout);
    runtime_cinematic_cutscene_fill();
    int current = -1; /* the screen the factor is set for, -1 none */
    int settle = 0;   /* loops left before the camera shows the factor last set */
    float rest[3] = {0}, last[3] = {0};
    int still = 0;
    for (;;) {
        usleep(4000);
        if (*(const uint64_t *)(uintptr_t)pass != pass_vtable) continue;
        const uint64_t camera = *(const uint64_t *)(uintptr_t)(pass + PASS_RENDER_CAMERA);
        if (camera < GUEST_MIN || camera >= GUEST_LIMIT || camera % 8) continue;
        const float *c = (const float *)(uintptr_t)camera;
        float pos[3];
        memcpy(pos, (const void *)(c + CAMERA_POSITION / 4), sizeof pos);
        if (position_offset) /* where the game put it, before our offset */
            for (int k = 0; k < 3; ++k) pos[k] -= position_offset[k];
        int screen = -1;
        for (int i = 0; i < SCREEN_COUNT && screen < 0; ++i)
            if (at(pos, &screens[i])) screen = i;
        if (screen < 0 && runtime_cinematic_cutscene()) screen = CUTSCENE;
        if (settle) --settle;
        if (screen == CUTSCENE && current == CUTSCENE && !settle) {
            /* A new shot: the camera's field of view was written with the factor now set. */
            const float factor = factor_for(c[CAMERA_FOV / 4] * DEGREES / *slot, c[CAMERA_ASPECT / 4]);
            if (factor > 0.0f && fabsf(factor - *slot) > 0.001f * DEGREES) {
                *slot = factor;
                settle = 8;
            }
        }
        if (screen != current) {
            if (position_offset) {
                for (int k = 0; k < 4; ++k) position_offset[k] = 0.0f;
                if (screen == 0 && c[CAMERA_ASPECT / 4] > 16.0f / 9.0f + 0.01f) {
                    const float *up = c + title_axis / 4;
                    for (int k = 0; k < 3; ++k) position_offset[k] = -title_lower * up[k];
                    printf("Frontend: title camera lowered by %.1f (row %#x)\n", title_lower, title_axis);
                    fflush(stdout);
                }
            }
            if (screen < 0) {
                *slot = DEGREES;
            } else {
                /* The game's own field of view: the factor last set divided out. */
                const float fov = c[CAMERA_FOV / 4] * DEGREES / *slot;
                const float factor = factor_for(fov, c[CAMERA_ASPECT / 4]);
                if (factor > 0.0f) {
                    *slot = factor;
                    printf("Frontend field of view: %s, %.3f -> %.3f degrees\n",
                           screen == CUTSCENE ? "cutscene" : screens[screen].name, fov, fov * factor / DEGREES);
                    fflush(stdout);
                }
            }
            settle = 8;
            current = screen;
        }
        if (trace) { /* where the camera comes to rest, once per rest */
            const int moved = fabsf(pos[0] - last[0]) + fabsf(pos[1] - last[1]) + fabsf(pos[2] - last[2]) > 0.01f;
            memcpy(last, pos, sizeof last);
            if (moved) still = 0;
            else if (++still == 250 && memcmp(rest, pos, sizeof pos)) {
                memcpy(rest, pos, sizeof rest);
                printf("Frontend field of view: camera at rest at %.3f %.3f %.3f, %.3f degrees\n", pos[0], pos[1],
                       pos[2], c[CAMERA_FOV / 4]);
                fflush(stdout);
            }
        }
    }
    return NULL;
}

/* The position hook: a page within jump range below the image with
 *   vaddps xmm3, xmm3, [rip+offset]; vmovups [rbx+0xa0], xmm3; jmp back
 * and the store replaced by a jump there. */
static void position_hook(const unsigned char *image, unsigned long long store_offset) {
    static const unsigned char store[8] = {0xc5, 0xf8, 0x11, 0x9b, 0xa0, 0x00, 0x00, 0x00};
    unsigned char *site = (unsigned char *)image + store_offset;
    if (memcmp(site, store, sizeof store)) {
        printf("Frontend: unexpected camera position store at %#llx; title camera unchanged\n", store_offset);
        return;
    }
    const long page = sysconf(_SC_PAGESIZE);
    unsigned char *cave = MAP_FAILED;
    for (int k = 1; k < 256 && cave == MAP_FAILED; ++k) {
        unsigned char *want = (unsigned char *)image - (uintptr_t)k * 0x10000;
        cave = mmap(want, (size_t)page, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (cave != MAP_FAILED && cave != want) { munmap(cave, (size_t)page); cave = MAP_FAILED; }
    }
    if (cave == MAP_FAILED) {
        printf("Frontend: no page near the image; title camera unchanged\n");
        return;
    }
    unsigned char *vec = cave + 0x40;
    unsigned char *p = cave;
    const int32_t to_vec = (int32_t)(vec - (cave + 8));
    *p++ = 0xc5; *p++ = 0xe0; *p++ = 0x58; *p++ = 0x1d; /* vaddps xmm3, xmm3, [rip+disp32] */
    memcpy(p, &to_vec, 4); p += 4;
    memcpy(p, store, sizeof store); p += sizeof store;
    const int64_t back = (int64_t)(uintptr_t)(site + 8) - (int64_t)(uintptr_t)(p + 5);
    const int64_t there = (int64_t)(uintptr_t)cave - (int64_t)(uintptr_t)(site + 5);
    if (back != (int32_t)back || there != (int32_t)there) {
        munmap(cave, (size_t)page);
        return;
    }
    *p++ = 0xe9;
    const int32_t b = (int32_t)back;
    memcpy(p, &b, 4);
    memset(vec, 0, 16);
    position_offset = (volatile float *)vec;
    unsigned char *first = (unsigned char *)((uintptr_t)site & ~(uintptr_t)(page - 1));
    const size_t length = (size_t)(site + 8 - first);
    if (mprotect(first, length, PROT_READ | PROT_WRITE | PROT_EXEC)) {
        position_offset = NULL;
        return;
    }
    unsigned char jump[8] = {0xe9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
    const int32_t t = (int32_t)there;
    memcpy(jump + 1, &t, 4);
    memcpy(site, jump, sizeof jump);
    mprotect(first, length, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)site, (char *)site + 8);
    __builtin___clear_cache((char *)cave, (char *)p + 4);
    const char *lower = getenv("BB_TITLE_LOWER"), *axis = getenv("BB_TITLE_AXIS");
    if (lower && *lower) title_lower = strtof(lower, NULL);
    if (axis && *axis) title_axis = (int)strtol(axis, NULL, 0);
}

/* Called before the game starts: points the field-of-view instruction at the slot. */
void runtime_frontend_start(const unsigned char *image) {
    const char *on = getenv("BB_FRONTEND_FOV");
    unsigned long long instruction = 0, vtable = 0, slot_offset = 0, position = 0;
    if (!on || sscanf(on, "%llx %llx %llx %llx", &instruction, &vtable, &slot_offset, &position) < 3) return;
    unsigned char *insn = (unsigned char *)image + instruction;
    /* vmulss xmm0, xmm0, [rip+disp32] reading 57.29578 */
    if (insn[0] != 0xc5 || insn[1] != 0xfa || insn[2] != 0x59 || insn[3] != 0x05) {
        printf("Frontend field of view: unexpected instruction at %#llx; off\n", instruction);
        return;
    }
    int32_t disp;
    memcpy(&disp, insn + 4, 4);
    float constant;
    memcpy(&constant, insn + 8 + disp, 4);
    if (fabsf(constant - DEGREES) > 0.001f) {
        printf("Frontend field of view: the instruction reads %f, not 57.29578; off\n", constant);
        return;
    }
    const int64_t new_disp = (int64_t)slot_offset - (int64_t)(instruction + 8);
    if (new_disp != (int32_t)new_disp) return;
    slot = (volatile float *)(image + slot_offset);
    *slot = DEGREES;
    const long page = sysconf(_SC_PAGESIZE);
    unsigned char *first = (unsigned char *)((uintptr_t)insn & ~(uintptr_t)(page - 1));
    const size_t length = (size_t)(insn + 8 - first);
    if (mprotect(first, length, PROT_READ | PROT_WRITE | PROT_EXEC)) {
        printf("Frontend field of view: could not patch the instruction; off\n");
        return;
    }
    const int32_t d = (int32_t)new_disp;
    memcpy(insn + 4, &d, 4);
    mprotect(first, length, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)insn, (char *)insn + 8);
    pass_vtable = (uint64_t)(uintptr_t)image + vtable;
    if (position) position_hook(image, position);
    const char *t = getenv("BB_FRONTEND_TRACE");
    trace = t && t[0] == '1';
    pthread_t thread;
    if (pthread_create(&thread, NULL, frontend_thread, NULL) == 0) pthread_detach(thread);
    printf("Frontend field of view: the title and customization screens and cutscenes fill the width\n");
}
#else
void runtime_frontend_start(const unsigned char *image) { (void)image; }
#endif
