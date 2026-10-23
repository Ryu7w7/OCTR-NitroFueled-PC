/* C interface of the GPU library (gpu/): shadPS4's Liverpool/Vulkan video core,
 * GnmDriver, VideoOut and kernel event queues, adapted to the native loader. */
#ifndef BBGPU_H
#define BBGPU_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    const char *title;          /* window title */
    const char *serial;         /* CUSA id, names the pipeline cache */
    const char *user_dir;       /* pipeline cache/logs directory */
    uint32_t sdk_version;       /* from the eboot's procparam */
    uint32_t psf_attributes;    /* param.sfo ATTRIBUTE */
    int32_t width, height;      /* initial window size */
    const char *icon;           /* window icon: the game's sce_sys/icon0.png (may be NULL) */
} BbGpuConfig;
/* Registers kernel event queues (needed with or without graphics). */
void bbgpu_register_kernel(void);
/* Creates window, Vulkan device, presenter and GPU command processor. */
int bbgpu_init(const BbGpuConfig *config);
/* Function for an imported NID ("NID#lib#mod"), or 0 when the GPU library does not provide it. */
uintptr_t bbgpu_resolve(const char *scoped_nid);
/* Called first by the loader's SIGSEGV handler: 1 when a GPU page-tracking fault was handled. */
int bbgpu_handle_fault(void *ucontext, void *address);
/* BB_WRITE_LOG=1: prints the logged GPU-side writes to guest memory near the fault. */
void bbgpu_dump_guest_writes(void *ucontext);
/* Keyboard text entry through the game window (IME dialog). begin returns 0 when
 * no window exists; poll returns 0 typing, 1 confirmed, 2 cancelled (UTF-8 text). */
int bbgpu_text_input_begin(const char *initial_utf8, const char *prompt_utf8);
int bbgpu_text_input_poll(char *out_utf8, uint64_t size);
/* 1 while the in-game settings menu is open: the game's pad input is held neutral. */
int bbgpu_overlay_captures_input(void);
/* SacredFire: 1 once after the in-game menu's "Rescan controllers" button (runtime_pad.c restarts
 * SDL's gamepad subsystem and enumerates devices afresh); cleared by the call. */
int bbgpu_pad_rescan_take(void);
/* SacredFire: what controller port 0..3 plays with now, for the in-game Controllers tab (issue
 * #50: identical controllers were one "x2" entry, and automatic ports showed "(none)"). name is
 * the gamepad's, "keyboard", or "" for none; pressing: a button or a stick is held. The pad
 * thread (runtime_pad.c) calls it when something changes. */
void bbgpu_pad_status(int port, const char *name_utf8, int pressing);
/* The player's game volume in percent, 0..200 (bbport.ini volume; the in-game menu changes it
 * while the game runs). runtime_audio.c reads it for every buffer. */
int bbgpu_audio_volume(void);
/* v0.4: a notice over the game for a few seconds (an unlocked trophy: its name, what it was for,
 * kind = its grade 1-4, its PNG picture or NULL). */
void bbgpu_notify(const char *title_utf8, const char *name_utf8, const char *detail_utf8, int kind,
                  const void *png, uint64_t png_size);
/* Patches the loaded image before the game runs (image still writable): libGnm entry hooks. */
void bbgpu_patch_image(unsigned char *image, uint64_t size);
/* Number of symbols registered by the vendored libraries (diagnostics). */
unsigned bbgpu_symbol_count(void);
#ifdef __cplusplus
}
#endif
#endif
