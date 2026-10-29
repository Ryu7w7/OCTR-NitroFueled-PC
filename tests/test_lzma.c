#define _GNU_SOURCE
#include "../src/runtime_lzma.c"
#include <assert.h>
#ifdef _WIN32
/* the same checks on Windows: an executable mapping and the environment */
static void *map_rwx(size_t size) { return VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE); }
static void unmap(void *p, size_t size) { (void)size; VirtualFree(p, 0, MEM_RELEASE); }
#define MAP_RWX_FAILED NULL
#define setenv(name, value, overwrite) _putenv_s(name, value)
#define unsetenv(name) _putenv_s(name, "")
#else
static void *map_rwx(size_t size) {
    return mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}
static void unmap(void *p, size_t size) { munmap(p, size); }
#define MAP_RWX_FAILED MAP_FAILED
#endif

int main(void) {
    unsigned char plain[32768], encoded[65536], output[32770];
    for (size_t i = 0; i < sizeof(plain); ++i) plain[i] = (unsigned char)(i * 17 ^ i / 31);
    size_t encoded_size = 0;
    for (unsigned marker = 0; marker <= 1; ++marker) {
        lzma_options_lzma options = {0};
        assert(!lzma_lzma_preset(&options, 1));
        options.dict_size = 32768;
        options.ext_flags = marker ? LZMA_LZMA1EXT_ALLOW_EOPM : 0;
        lzma_filter filters[] = {{LZMA_FILTER_LZMA1EXT, &options}, {LZMA_VLI_UNKNOWN, NULL}};
        encoded_size = 0;
        assert(lzma_raw_buffer_encode(filters, NULL, plain, sizeof(plain), encoded,
                                     &encoded_size, sizeof(encoded)) == LZMA_OK);
        memset(output, 0xa5, sizeof(output));
        assert(runtime_lzma_block(3, 0, 2, 32768, encoded, encoded_size, output + 1, sizeof(plain)));
        assert(!memcmp(output + 1, plain, sizeof(plain)));
        assert(output[0] == 0xa5 && output[sizeof(output) - 1] == 0xa5);
        assert(!runtime_lzma_block(3, 0, 2, 32768, encoded, encoded_size / 2, output + 1, sizeof(plain)));
        encoded[0] ^= 0xff;
        assert(!runtime_lzma_block(3, 0, 2, 32768, encoded, encoded_size, output + 1, sizeof(plain)));
        encoded[0] ^= 0xff;
    }
    assert(!runtime_lzma_block(8, 4, 2, 32768, encoded, encoded_size, output, 32768));
    assert(!runtime_lzma_block(3, 0, 2, 32768, encoded, encoded_size, output, 32769));

    /* Execute the patched call and fallback, not merely inspect the written jump bytes. */
    const size_t size = 0x1586000, entry = 0x1584ed0, call = 0x14f46ca;
    unsigned char *image = map_rwx(size);
    assert(image != MAP_RWX_FAILED);
    /* Worker registration is signature-gated and leaves smaller hosts untouched. */
    const size_t workers = 0x14e6548;
    const unsigned char worker_code[] = {
        0xbe,0xc0,0x03,0,0,0xba,0x64,0,0,0,0x48,0x89,0xdf,0xe8,0x56,0x0b,0x0a,0
    };
    expand_decode_workers(image, size, 12);
    assert(image[workers] == 0);
    memcpy(image + workers, worker_code, sizeof(worker_code));
    expand_decode_workers(image, size, 4);
    assert(!memcmp(image + workers, worker_code, sizeof(worker_code)));
    setenv("BB_LZMA_WORKERS", "4", 1); /* the player's opt-out keeps the game's four */
    expand_decode_workers(image, size, 16);
    assert(!memcmp(image + workers, worker_code, sizeof(worker_code)));
    unsetenv("BB_LZMA_WORKERS");
    expand_decode_workers(image, workers, 12);
    assert(!memcmp(image + workers, worker_code, sizeof(worker_code)));
    expand_decode_workers(image, size, 12);
    assert(image[workers + 1] == 0xc0 && image[workers + 2] == 0x0f);
    assert(!memcmp(image + workers + 3, worker_code + 3, sizeof(worker_code) - 3));
    runtime_lzma_patch(image, size);
    assert(!original_decode); /* unknown image */
    memcpy(image + entry, "\x55\x48\x89\xe5\x41\x57\x41\x56\x41\x55\x41\x54\x53\x48\x83\xec\x78", 17);
    memcpy(image + entry + 17, "\xb8\x4d\0\0\0\x48\x83\xc4\x78\x5b\x41\x5c\x41\x5d\x41\x5e\x41\x5f\x5d\xc3", 20);
    memcpy(image + call, "\xe8\x01\x08\x09\0\x48\x8b\xb5\x58\xff\xff\xff", 12);
    memcpy(image + 0x14f4630, "\x83\xf9\x02\x0f\x85\x4f\x01\0\0", 9);
    setenv("BB_NATIVE_LZMA", "0", 1);
    runtime_lzma_patch(image, size);
    assert(!original_decode);
    unsetenv("BB_NATIVE_LZMA");
    setenv("BB_LZMA_CACHE_MB", "64", 1);
    runtime_lzma_patch(image, size);
    assert(original_decode);
    memcpy(image + call - 4, "\x48\x83\xec\x08", 4);
    memcpy(image + call + 5, "\x48\x83\xc4\x08\xc3", 5);
    unsigned properties[] = {3, 0, 2, 32768};
    size_t in_size = encoded_size, out_size = sizeof(plain);
    int status = 0;
    Decode run = (Decode)(image + call - 4);
    assert(run(properties, output, &out_size, encoded, &in_size, &status) == 0);
    assert(!memcmp(output, plain, sizeof(plain)));
    assert(in_size == encoded_size && out_size == sizeof(plain));
    assert(atomic_load(&decoded_blocks) == 1);
    /* the same block again: copied from the RAM cache, byte for byte, without decoding */
    memset(output, 0, sizeof(output));
    assert(run(properties, output, &out_size, encoded, &in_size, &status) == 0);
    assert(!memcmp(output, plain, sizeof(plain)));
    assert(atomic_load(&decoded_blocks) == 1 && atomic_load(&cached_blocks) == 1);
    /* a different block of the same size is not mistaken for it */
    encoded[encoded_size - 1] ^= 1;
    memset(output, 0, sizeof(output));
    run(properties, output, &out_size, encoded, &in_size, &status);
    assert(atomic_load(&cached_blocks) == 1);
    encoded[encoded_size - 1] ^= 1;
    properties[0] = 8;
    assert(run(properties, output, &out_size, encoded, &in_size, &status) == 77);
    assert(atomic_load(&fallback_blocks) == 2); /* the corrupted block above, then this one */
    /* A full cache drops its oldest blocks: 3000 distinct 32 KiB blocks into 64 MiB (2048 fit). */
    static unsigned char block[32768], back[32768];
    for (unsigned i = 0; i < 3000; ++i) {
        memset(block, (int)(i & 0xff), sizeof(block));
        memcpy(block, &i, sizeof(i));
        cache_put(1000 + i, ~(uint64_t)i, block, sizeof(block));
        assert(cache_bytes <= cache_budget);
    }
    assert(atomic_load(&cache_evicted) >= 3000 - 2048 && cache_ring_count <= 2048);
    assert(!cache_get(1000, ~(uint64_t)0, sizeof(back), back)); /* the oldest went */
    const unsigned last = 2999;
    assert(cache_get(1000 + last, ~(uint64_t)last, sizeof(back), back));
    assert(!memcmp(back, &last, sizeof(last)) && back[sizeof(back) - 1] == (last & 0xff));
    assert(!cache_get(1000 + last, ~(uint64_t)last, 16384, back)); /* the size is part of the key */
    runtime_lzma_report();
    unmap(image, size);
    unmap((void *)original_decode, 4096);
    puts("LZMA tests passed: markers, bounds, corruption, signature, opt-out, native hook and fallback");
    return 0;
}
