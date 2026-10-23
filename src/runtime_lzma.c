/* CTR's independent 32 KiB archive blocks. No disk cache or modified PAKs. */
#define _GNU_SOURCE
#include "runtime.h"
#ifndef _WIN32
#include <lzma.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

typedef int (ABI *Decode)(void *, void *, size_t *, const void *, size_t *, int *);
static Decode original_decode;
static const void *archive_return;
static atomic_ulong decoded_blocks, fallback_blocks;

/* Exposed to the byte-for-byte decoder test; also rejects unsupported properties. */
int runtime_lzma_block(unsigned lc, unsigned lp, unsigned pb, unsigned dictionary,
                       const void *input, size_t input_size, void *output, size_t output_size) {
    if (lc > 8 || lp > 4 || pb > 4 || lc + lp > 4 || !input_size || input_size > 65536 ||
        !output_size || output_size > 32768) return 0;
    lzma_options_lzma options = {0};
    options.lc = lc; options.lp = lp; options.pb = pb;
    /* No previous block is referenced: a dictionary larger than the output is unnecessary. */
    if (dictionary > output_size) dictionary = (unsigned)output_size;
    options.dict_size = dictionary < 4096 ? 4096 : dictionary;
    options.ext_flags = LZMA_LZMA1EXT_ALLOW_EOPM;
    options.ext_size_low = (uint32_t)output_size;
    lzma_filter filters[] = {{LZMA_FILTER_LZMA1EXT, &options}, {LZMA_VLI_UNKNOWN, NULL}};
    size_t in_pos = 0, out_pos = 0;
    lzma_ret result = lzma_raw_buffer_decode(filters, NULL, input, &in_pos, input_size,
                                            output, &out_pos, output_size);
    return result == LZMA_OK && in_pos == input_size && out_pos == output_size;
}

static ABI __attribute__((noinline)) int decode_block(void *state, void *output, size_t *output_size,
                                       const void *input, size_t *input_size, int *status) {
    unsigned properties[4];
    memcpy(properties, state, sizeof(properties));
    /* Only this verified one-shot caller discards the decoder state immediately afterwards.
     * Streaming callers must retain the original stateful decoder. */
    if (__builtin_return_address(0) == archive_return &&
        runtime_lzma_block(properties[0], properties[1], properties[2], properties[3],
                           input, *input_size, output, *output_size)) {
        *status = 4; /* MAYBE_FINISHED_WITHOUT_MARK; the archive caller checks sizes, not status. */
        if (atomic_fetch_add_explicit(&decoded_blocks, 1, memory_order_relaxed) == 0)
            puts("Archive decoder: first LZMA block decoded by liblzma (original PAK, no disk cache)");
        return 0;
    }
    atomic_fetch_add_explicit(&fallback_blocks, 1, memory_order_relaxed);
    return original_decode(state, output, output_size, input, input_size, status);
}

static void absolute_jump(unsigned char *where, const void *target) {
    /* RIP-indirect jump preserves every argument and general register. */
    memcpy(where, "\xff\x25\0\0\0\0", 6);
    memcpy(where + 6, &target, 8);
}

static void expand_decode_workers(unsigned char *image, size_t size, long cpus) {
    enum { WORKERS = 0x14e6548 };
    static const unsigned char signature[] = {
        0xbe,0xc0,0x03,0x00,0x00,0xba,0x64,0x00,0x00,0x00,
        0x48,0x89,0xdf,0xe8,0x56,0x0b,0x0a,0x00
    };
    /* Register the queue with six existing low-priority workers before any jobs are queued. */
    if (cpus < 12 || size < WORKERS + sizeof(signature) ||
        memcmp(image + WORKERS, signature, sizeof(signature))) return;
    uint32_t mask = 0xfc0; /* Worker bits 6..11; preserve the main/gameplay workers. */
    memcpy(image + WORKERS + 1, &mask, sizeof(mask));
    puts("Archive decoder: six existing streaming workers enabled (originally four)");
}

void runtime_lzma_patch(unsigned char *image, size_t size) {
    const char *setting = getenv("BB_NATIVE_LZMA");
    if (setting && !strcmp(setting, "0")) return;
    enum { ENTRY = 0x1584ed0, CALL = 0x14f46ca, PREFIX = 17 };
    static const unsigned char prefix[PREFIX] = {
        0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x78
    };
    static const unsigned char call[] = {0xe8,0x01,0x08,0x09,0x00,0x48,0x8b,0xb5,0x58,0xff,0xff,0xff};
    static const unsigned char setup[] = {0x83,0xf9,0x02,0x0f,0x85,0x4f,0x01,0x00,0x00};
    /* Fail closed on other games/builds. Verified CTR 01.21 LZMA call and ABI. */
    if (original_decode || size < ENTRY + PREFIX || size < CALL + sizeof(call) ||
        memcmp(image + ENTRY, prefix, PREFIX) || memcmp(image + CALL, call, sizeof(call)) ||
        memcmp(image + 0x14f4630, setup, sizeof(setup))) return;
    unsigned char *trampoline = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (trampoline == MAP_FAILED) return;
    /* The copied prologue contains no RIP-relative instructions. */
    memcpy(trampoline, prefix, PREFIX);
    absolute_jump(trampoline + PREFIX, image + ENTRY + PREFIX);
    if (mprotect(trampoline, 4096, PROT_READ | PROT_EXEC)) { munmap(trampoline, 4096); return; }
    original_decode = (Decode)trampoline;
    archive_return = image + CALL + 5;
    absolute_jump(image + ENTRY, (void *)decode_block);
    expand_decode_workers(image, size, sysconf(_SC_NPROCESSORS_ONLN));
    puts("Archive decoder: native liblzma enabled for CTR 01.21 (BB_NATIVE_LZMA=0 disables)");
}

void runtime_lzma_report(void) {
    if (original_decode) printf("Archive decoder: %lu native blocks, %lu original fallbacks\n",
        atomic_load_explicit(&decoded_blocks, memory_order_relaxed),
        atomic_load_explicit(&fallback_blocks, memory_order_relaxed));
}
#else
void runtime_lzma_patch(unsigned char *image, size_t size) { (void)image; (void)size; }
void runtime_lzma_report(void) {}
#endif
