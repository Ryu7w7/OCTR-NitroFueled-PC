/* CTR's independent 32 KiB archive blocks. No disk cache or modified PAKs.
 * Loading screens are CPU-bound in the game's own LZMA decoder (its four job workers at ~90% for
 * the whole ~18 s load); liblzma decodes the same blocks about twice as fast. Linux and Windows
 * (there liblzma is linked statically from MSYS2's xz). */
#define _GNU_SOURCE
#include "runtime.h"
#ifdef _WIN32
#ifndef LZMA_API_STATIC
#define LZMA_API_STATIC
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
#include <lzma.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int (ABI *Decode)(void *, void *, size_t *, const void *, size_t *, int *);
static Decode original_decode;
static const void *archive_return;
static atomic_ulong decoded_blocks, fallback_blocks, cached_blocks;

/* ---- decoded blocks kept in RAM -------------------------------------------------------------
 * A load decodes the same blocks again and again (the title screen between races, characters,
 * karts and the HUD of every race, a restarted track). Each decoded block is kept under a 128-bit
 * hash of its compressed bytes, its properties and its size; a later identical block is copied
 * (hashing 32 KiB costs a few microseconds, decoding it a few hundred). Oldest blocks go first
 * when the budget is full. BB_LZMA_CACHE_MB sets the budget (0: off); by default 256 MiB below 12 GB
 * of RAM, 768 MiB below 24 GB, else 1 GiB. */
typedef struct {
    uint64_t a, b;
    uint32_t size;
    int next;            /* next entry in the bucket, -1 */
    unsigned char *data; /* NULL: a free entry */
} CacheEntry;
static pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;
static CacheEntry *cache_entries;
static int *cache_buckets, *cache_ring; /* ring: entries in the order they were added */
static int *cache_free, cache_free_count; /* entries not in use */
static int cache_capacity, cache_bucket_mask, cache_ring_head, cache_ring_count;
static uint64_t cache_budget, cache_bytes;
static atomic_ulong cache_evicted;

static uint64_t mix64(uint64_t x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ull;
    return x ^ (x >> 33);
}

/* Two independent 64-bit lanes over the bytes (8 at a time), seeded with the properties. */
static void block_hash(const unsigned char *p, size_t n, uint64_t seed, uint64_t *a, uint64_t *b) {
    uint64_t h1 = 0x9e3779b97f4a7c15ull ^ seed ^ n, h2 = 0xd6e8feb86659fd93ull ^ mix64(seed + n);
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h1 = (h1 ^ mix64(w)) * 0x9fb21c651e98df25ull;
        h2 = (h2 + w) * 0xbf58476d1ce4e5b9ull;
        h2 ^= h2 >> 31;
    }
    uint64_t tail = 0;
    memcpy(&tail, p + i, n - i);
    *a = mix64(h1 ^ tail ^ 0x2545f4914f6cdd1dull);
    *b = mix64(h2 + tail * 0x94d049bb133111ebull);
}

static uint64_t ram_bytes(void) {
#ifdef _WIN32
    MEMORYSTATUSEX m = {.dwLength = sizeof m};
    return GlobalMemoryStatusEx(&m) ? m.ullTotalPhys : 0;
#else
    const long pages = sysconf(_SC_PHYS_PAGES), page = sysconf(_SC_PAGESIZE);
    return pages > 0 && page > 0 ? (uint64_t)pages * (uint64_t)page : 0;
#endif
}

static void cache_init(void) {
    const char *setting = getenv("BB_LZMA_CACHE_MB");
    /* the game itself needs most of an 8 GB PC: less RAM, a smaller budget */
    const uint64_t ram_gb = ram_bytes() >> 30;
    uint64_t mb = setting && *setting ? strtoull(setting, NULL, 10)
                  : ram_gb >= 24 ? 1024 : ram_gb >= 12 ? 768 : ram_gb >= 6 ? 256 : 0;
    if (mb > 4096) mb = 4096;
    if (mb < 32) return; /* off (or too little RAM to spare) */
    cache_budget = mb << 20;
    /* blocks decode to at most 32 KiB; small ones are rare: room for 2 entries per 32 KiB */
    cache_capacity = (int)(cache_budget / (16u << 10));
    int buckets = 1;
    while (buckets < cache_capacity) buckets <<= 1;
    cache_entries = calloc((size_t)cache_capacity, sizeof *cache_entries);
    cache_buckets = malloc((size_t)buckets * sizeof *cache_buckets);
    cache_ring = malloc((size_t)cache_capacity * sizeof *cache_ring);
    cache_free = malloc((size_t)cache_capacity * sizeof *cache_free);
    if (!cache_entries || !cache_buckets || !cache_ring || !cache_free) {
        free(cache_entries); free(cache_buckets); free(cache_ring); free(cache_free);
        cache_entries = NULL; cache_budget = 0;
        return;
    }
    for (int i = 0; i < buckets; ++i) cache_buckets[i] = -1;
    for (int i = 0; i < cache_capacity; ++i) cache_free[i] = cache_capacity - 1 - i;
    cache_free_count = cache_capacity;
    cache_bucket_mask = buckets - 1;
    printf("Archive decoder: %llu MiB of RAM keep decoded blocks for the next loads (BB_LZMA_CACHE_MB)\n",
           (unsigned long long)mb);
}

/* Removes the oldest entry (cache_lock held). */
static void cache_evict_oldest(void) {
    const int e = cache_ring[cache_ring_head];
    cache_ring_head = (cache_ring_head + 1) % cache_capacity;
    --cache_ring_count;
    CacheEntry *entry = &cache_entries[e];
    int *link = &cache_buckets[entry->a & (uint64_t)cache_bucket_mask];
    while (*link != -1 && *link != e) link = &cache_entries[*link].next;
    if (*link == e) *link = entry->next;
    cache_bytes -= entry->size;
    free(entry->data);
    entry->data = NULL;
    cache_free[cache_free_count++] = e;
    atomic_fetch_add_explicit(&cache_evicted, 1, memory_order_relaxed);
}

/* Copies a kept block into `out`; 1 when it was there. */
static int cache_get(uint64_t a, uint64_t b, size_t size, void *out) {
    int found = 0;
    pthread_mutex_lock(&cache_lock);
    for (int e = cache_buckets[a & (uint64_t)cache_bucket_mask]; e != -1; e = cache_entries[e].next) {
        const CacheEntry *entry = &cache_entries[e];
        if (entry->a == a && entry->b == b && entry->size == size) {
            memcpy(out, entry->data, size);
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&cache_lock);
    return found;
}

static void cache_put(uint64_t a, uint64_t b, const void *data, size_t size) {
    unsigned char *copy = malloc(size);
    if (!copy) return;
    memcpy(copy, data, size);
    pthread_mutex_lock(&cache_lock);
    for (int e = cache_buckets[a & (uint64_t)cache_bucket_mask]; e != -1; e = cache_entries[e].next) {
        if (cache_entries[e].a == a && cache_entries[e].b == b && cache_entries[e].size == size) {
            pthread_mutex_unlock(&cache_lock); /* another worker kept it meanwhile */
            free(copy);
            return;
        }
    }
    while (cache_ring_count > 0 && (cache_ring_count == cache_capacity || cache_bytes + size > cache_budget))
        cache_evict_oldest();
    if (!cache_free_count) {
        pthread_mutex_unlock(&cache_lock);
        free(copy);
        return;
    }
    const int e = cache_free[--cache_free_count];
    CacheEntry *entry = &cache_entries[e];
    entry->a = a; entry->b = b; entry->size = (uint32_t)size; entry->data = copy;
    int *bucket = &cache_buckets[a & (uint64_t)cache_bucket_mask];
    entry->next = *bucket;
    *bucket = e;
    cache_ring[(cache_ring_head + cache_ring_count) % cache_capacity] = e;
    ++cache_ring_count;
    cache_bytes += size;
    pthread_mutex_unlock(&cache_lock);
}

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
    if (__builtin_return_address(0) == archive_return) {
        uint64_t a = 0, b = 0;
        const int cached = cache_entries && *input_size <= 65536 && *output_size <= 32768;
        if (cached) {
            const uint64_t seed = ((uint64_t)properties[0] << 56) ^ ((uint64_t)properties[1] << 48) ^
                                  ((uint64_t)properties[2] << 40) ^ ((uint64_t)properties[3] << 8) ^
                                  (uint64_t)*output_size;
            block_hash(input, *input_size, seed, &a, &b);
            if (cache_get(a, b, *output_size, output)) {
                *status = 4;
                atomic_fetch_add_explicit(&cached_blocks, 1, memory_order_relaxed);
                return 0;
            }
        }
        if (runtime_lzma_block(properties[0], properties[1], properties[2], properties[3],
                               input, *input_size, output, *output_size)) {
            *status = 4; /* MAYBE_FINISHED_WITHOUT_MARK; the archive caller checks sizes, not status. */
            if (atomic_fetch_add_explicit(&decoded_blocks, 1, memory_order_relaxed) == 0)
                puts("Archive decoder: first LZMA block decoded by liblzma (original PAK, no disk cache)");
            if (cached) cache_put(a, b, output, *output_size);
            return 0;
        }
    }
    atomic_fetch_add_explicit(&fallback_blocks, 1, memory_order_relaxed);
    return original_decode(state, output, output_size, input, input_size, status);
}

static void absolute_jump(unsigned char *where, const void *target) {
    /* RIP-indirect jump preserves every argument and general register. */
    memcpy(where, "\xff\x25\0\0\0\0", 6);
    memcpy(where + 6, &target, 8);
}

static void expand_decode_workers(unsigned char *image, size_t size, long cpus, size_t shift) {
    const size_t WORKERS = 0x14e6548 - shift;
    static const unsigned char signature[] = {
        0xbe,0xc0,0x03,0x00,0x00,0xba,0x64,0x00,0x00,0x00,
        0x48,0x89,0xdf,0xe8,0x56,0x0b,0x0a,0x00
    };
    /* Register the queue with six existing low-priority workers before any jobs are queued.
     * From 8 hardware threads (12 in PR #115's first version; loads are where they work).
     * BB_LZMA_WORKERS=4 keeps the game's four. */
    const char *workers = getenv("BB_LZMA_WORKERS");
    if ((workers && !strcmp(workers, "4")) || cpus < 8 || size < WORKERS + sizeof(signature) ||
        memcmp(image + WORKERS, signature, sizeof(signature))) return;
    uint32_t mask = 0xfc0; /* Worker bits 6..11; preserve the main/gameplay workers. */
    memcpy(image + WORKERS + 1, &mask, sizeof(mask));
    puts("Archive decoder: six existing streaming workers enabled (originally four)");
}

void runtime_lzma_patch(unsigned char *image, size_t size) {
    const char *setting = getenv("BB_NATIVE_LZMA");
    if (setting && !strcmp(setting, "0")) return;
    enum { PREFIX = 17 };
    static const unsigned char prefix[PREFIX] = {
        0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x78
    };
    static const unsigned char call[] = {0xe8,0x01,0x08,0x09,0x00,0x48,0x8b,0xb5,0x58,0xff,0xff,0xff};
    static const unsigned char setup[] = {0x83,0xf9,0x02,0x0f,0x85,0x4f,0x01,0x00,0x00};
    /* Fail closed on other games/builds. Verified CTR 01.21 LZMA call and ABI: USA (CUSA13795)
     * at these addresses, EU (CUSA14876) with the same code 0x10 earlier. */
    static const size_t shifts[] = {0, 0x10};
    size_t ENTRY = 0, CALL = 0, shift = 0;
    int found = 0;
    for (size_t i = 0; !found && i < sizeof(shifts) / sizeof(shifts[0]); i++) {
        shift = shifts[i];
        ENTRY = 0x1584ed0 - shift;
        CALL = 0x14f46ca - shift;
        found = size >= ENTRY + PREFIX && size >= CALL + sizeof(call) &&
                !memcmp(image + ENTRY, prefix, PREFIX) && !memcmp(image + CALL, call, sizeof(call)) &&
                !memcmp(image + 0x14f4630 - shift, setup, sizeof(setup));
    }
    if (original_decode || !found) return;
#ifdef _WIN32
    unsigned char *trampoline = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!trampoline) return;
#else
    unsigned char *trampoline = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (trampoline == MAP_FAILED) return;
#endif
    /* The copied prologue contains no RIP-relative instructions. */
    memcpy(trampoline, prefix, PREFIX);
    absolute_jump(trampoline + PREFIX, image + ENTRY + PREFIX);
#ifdef _WIN32
    DWORD old;
    if (!VirtualProtect(trampoline, 4096, PAGE_EXECUTE_READ, &old)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return;
    }
    FlushInstructionCache(GetCurrentProcess(), trampoline, 4096);
#else
    if (mprotect(trampoline, 4096, PROT_READ | PROT_EXEC)) { munmap(trampoline, 4096); return; }
#endif
    original_decode = (Decode)trampoline;
    archive_return = image + CALL + 5;
    absolute_jump(image + ENTRY, (void *)decode_block);
    cache_init();
#ifdef _WIN32
    expand_decode_workers(image, size, (long)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS), shift);
#else
    expand_decode_workers(image, size, sysconf(_SC_NPROCESSORS_ONLN), shift);
#endif
    printf("Archive decoder: native liblzma enabled for CTR %s 1.21 (BB_NATIVE_LZMA=0 disables)\n",
           shift ? "EU" : "USA");
}

void runtime_lzma_report(void) {
    if (original_decode) printf("Archive decoder: %lu native blocks, %lu from the RAM cache (%lu dropped, "
        "%llu MiB kept), %lu original fallbacks\n",
        (unsigned long)atomic_load_explicit(&decoded_blocks, memory_order_relaxed),
        (unsigned long)atomic_load_explicit(&cached_blocks, memory_order_relaxed),
        (unsigned long)atomic_load_explicit(&cache_evicted, memory_order_relaxed),
        (unsigned long long)(cache_bytes >> 20),
        (unsigned long)atomic_load_explicit(&fallback_blocks, memory_order_relaxed));
}
