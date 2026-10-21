/* SacredFire: in ultrawide the game draws everything at the monitor's width. Bink videos are
 * 16:9 pictures the game stretches to fill it, and cutscenes are staged for 16:9 (the sides show
 * off-stage). With BB_CINEMATIC_16X9=1 (run.sh: ultrawide on, bbport.ini ultrawide_cinematics not
 * 0) the presenter shows them at 16:9 with black bars: runtime_cinematic_mode() is 1 while a video
 * file (.bk2) is open (the frame is squeezed back to 16:9) and 2 while the cutscene manager has an
 * active cutscene (the middle 16:9 of the frame is shown). The camera fly-over before a race (the
 * track's name over gameplay graphics) is a cutscene too, staged for any width: CCutsceneData's
 * _isFlybyIntro marks it, and it stays ultrawide. BB_CUTSCENE="INSTANCE FIELD FIELD [FLYBY]"
 * (image offset of CCutsceneManager::_Instance, the offsets of _activeCutsceneData and
 * _activeDesignerCutscene, and of CCutsceneData::_isFlybyIntro; scripts/patches.py
 * --print-cutscene) names the cutscene state. With the frontend field of view on (runtime_frontend.c,
 * Linux) cutscenes get a 16:9 field of view instead of the crop. */
#include "runtime.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define GUEST_MIN UINT64_C(0x1000000000)
#define GUEST_LIMIT (UINT64_C(1) << 40)

static int enabled;
static const unsigned char *instance_slot; /* &CCutsceneManager::_Instance in the image */
static unsigned active_field, designer_field;
static int flyby_field = -1; /* CCutsceneData::_isFlybyIntro, -1 when not found */
static int videos; /* open .bk2 files */

void runtime_cinematic_start(const unsigned char *image) {
    const char *on = getenv("BB_CINEMATIC_16X9");
    if (!on || on[0] != '1') return;
    enabled = 1;
    const char *cutscene = getenv("BB_CUTSCENE");
    unsigned long long slot = 0;
    unsigned flyby = 0;
    const int found = cutscene ? sscanf(cutscene, "%llx %x %x %x", &slot, &active_field, &designer_field, &flyby) : 0;
    if (found >= 3 && slot) {
        instance_slot = image + slot;
        if (found == 4) flyby_field = (int)flyby;
    }
    printf("Ultrawide: videos%s at 16:9%s\n", instance_slot ? " and cutscenes" : "",
           flyby_field >= 0 ? " (race fly-over intros stay ultrawide)" : "");
}

void runtime_cinematic_video(int delta) {
    __atomic_add_fetch(&videos, delta, __ATOMIC_RELAXED);
}

static uint64_t guest_qword(uint64_t address) {
    if (address < GUEST_MIN || address >= GUEST_LIMIT || address % 8) return 0;
    return __atomic_load_n((const uint64_t *)(uintptr_t)address, __ATOMIC_RELAXED);
}

/* A cutscene plays (not the fly-over before a race). */
int runtime_cinematic_cutscene(void) {
    if (!enabled || !instance_slot) return 0;
    const uint64_t manager = __atomic_load_n((const uint64_t *)(const void *)instance_slot, __ATOMIC_RELAXED);
    if (manager < GUEST_MIN || manager >= GUEST_LIMIT) return 0;
    const uint64_t active = guest_qword(manager + active_field);
    if (active && flyby_field >= 0 &&
        __atomic_load_n((const unsigned char *)(uintptr_t)(active + (unsigned)flyby_field), __ATOMIC_RELAXED))
        return 0; /* the fly-over before a race: gameplay graphics, ultrawide */
    return active || guest_qword(manager + designer_field) ? 1 : 0;
}

/* Set by runtime_frontend.c once it gives cutscenes a 16:9 field of view: the 3D then fills the
 * width as a 16:9 view would, and the frame is not cropped (the cutscene's subtitles and prompts
 * are drawn across the whole width; a crop cut them off). */
static int cutscene_fill;
void runtime_cinematic_cutscene_fill(void) {
    __atomic_store_n(&cutscene_fill, 1, __ATOMIC_RELAXED);
}

/* 0 normal, 1 video (squeeze to 16:9), 2 cutscene (crop to 16:9). Called by the presenter. */
int runtime_cinematic_mode(void) {
    if (!enabled) return 0;
    if (__atomic_load_n(&videos, __ATOMIC_RELAXED) > 0) return 1;
    if (__atomic_load_n(&cutscene_fill, __ATOMIC_RELAXED)) return 0;
    return runtime_cinematic_cutscene() ? 2 : 0;
}
