/* The port's own content (new tracks: their level archive, their preview video) kept encrypted on disk, so it
 * cannot be taken out of the files with archive tools (27/11: Neo Bowser City, modified for the port with its
 * author's approval). The game reads it through the port's file layer (runtime_file.c), decrypted in memory as
 * it reads; nothing decrypted is written to disk.
 *
 * ChaCha20 (RFC 7539: 32-bit block counter, 96-bit nonce): byte i of a file is XORed with byte i % 64 of block
 * i / 64 of the file's own key stream, so any range decrypts on its own. tools/content_crypt.py encrypts
 * (Python's cryptography package, the same stream: its 16-byte nonce is the counter then ours).
 *
 * Trailer, the file's last 48 bytes (the file's own size before it):
 *   "OCTRENC1", 12 bytes nonce, u32 prefix, u64 tail_at, u32 tail_len, u32 0, u64 size (little endian)
 * [0, prefix) and [tail_at, tail_at + tail_len) are left as they are: an archive's index and names, which the
 * port's scripts read (scripts/track_content.py) to hide and rename entries; every file's data is encrypted.
 *
 * The key is not in the source: official builds get it from a file kept outside the repository
 * (gpu/CMakeLists.txt, SACREDFIRE_CONTENT_KEY_FILE), stored in the exe split in two masked halves. A build
 * without it reads every unencrypted file as before and refuses the encrypted ones.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _WIN32
#define _GNU_SOURCE   /* pread with -std=c11 (build.sh, gcc) */
#endif
#include "runtime_content_crypt.h"
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef _WIN32
#include "compat_win.h"   /* pread on CRT descriptors */
#endif
#if defined(OCTR_CONTENT_KEY) && defined(__has_include)
#if __has_include("content_key.h")
#include "content_key.h"   /* generated: content_key_a[32] ^ content_key_b[32] */
#define HAVE_CONTENT_KEY 1
#endif
#endif

static const uint8_t trailer_magic[8] = {'O', 'C', 'T', 'R', 'E', 'N', 'C', '1'};

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }

int runtime_content_crypt_parse(const uint8_t trailer[CONTENT_CRYPT_TRAILER], uint64_t file_size, ContentCrypt *c) {
    if (file_size < CONTENT_CRYPT_TRAILER || memcmp(trailer, trailer_magic, sizeof trailer_magic)) return 0;
    ContentCrypt t = {0};
    memcpy(t.nonce, trailer + 8, sizeof t.nonce);
    t.prefix = le32(trailer + 20);
    t.tail_at = le64(trailer + 24);
    t.tail_len = le32(trailer + 32);
    t.size = le64(trailer + 40);
    if (t.size != file_size - CONTENT_CRYPT_TRAILER || t.prefix > t.size || t.tail_at > t.size ||
        t.tail_len > t.size - t.tail_at || le32(trailer + 36))
        return -1;
    t.on = 1;
    *c = t;
    return 1;
}

#ifdef HAVE_CONTENT_KEY
static uint32_t key_words[8];
static int key_state;   /* 0 not made yet, 1 made */
static void make_key(void) {
    if (__atomic_load_n(&key_state, __ATOMIC_ACQUIRE)) return;
    /* Read through volatile: with both halves constant the compiler folded their XOR and the exe held the key
     * itself (found 27/11 by looking for its bytes in the exe). */
    const volatile unsigned char *a = content_key_a, *m = content_key_b;
    uint8_t key[32];
    for (int i = 0; i < 32; ++i) key[i] = (uint8_t)(a[i] ^ m[i]);
    uint32_t words[8];
    for (int i = 0; i < 8; ++i) words[i] = le32(key + 4 * i);
    memcpy(key_words, words, sizeof key_words);
    memset(key, 0, sizeof key);
    __atomic_store_n(&key_state, 1, __ATOMIC_RELEASE);
}
#endif

int runtime_content_crypt_key_ready(void) {
#ifdef HAVE_CONTENT_KEY
    return 1;
#else
    return 0;
#endif
}

int runtime_content_crypt_probe(int fd, uint64_t file_size, ContentCrypt *c) {
    memset(c, 0, sizeof *c);
    if (fd < 0 || file_size < CONTENT_CRYPT_TRAILER) return 0;
    uint8_t trailer[CONTENT_CRYPT_TRAILER];
    if (pread(fd, trailer, sizeof trailer, (off_t)(file_size - CONTENT_CRYPT_TRAILER)) != (ssize_t)sizeof trailer)
        return 0;
    const int r = runtime_content_crypt_parse(trailer, file_size, c);
    if (r > 0 && !runtime_content_crypt_key_ready()) {
        c->on = 0;
        return -1;
    }
    return r;
}

#define ROTL(a, b) (((a) << (b)) | ((a) >> (32 - (b))))
#define QR(a, b, c, d) (a += b, d ^= a, d = ROTL(d, 16), c += d, b ^= c, b = ROTL(b, 12), \
                        a += b, d ^= a, d = ROTL(d, 8), c += d, b ^= c, b = ROTL(b, 7))
/* One 64-byte block of the key stream (RFC 7539 2.3). */
/* unused in a build without the key (the decrypt below is left out) */
__attribute__((unused)) static void chacha_block(const uint32_t key[8], uint32_t counter, const uint8_t nonce[12], uint8_t out[64]) {
    const uint32_t in[16] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574,
                             key[0], key[1], key[2], key[3], key[4], key[5], key[6], key[7],
                             counter, le32(nonce), le32(nonce + 4), le32(nonce + 8)};
    uint32_t x[16];
    memcpy(x, in, sizeof x);
    for (int i = 0; i < 10; ++i) {
        QR(x[0], x[4], x[8], x[12]); QR(x[1], x[5], x[9], x[13]); QR(x[2], x[6], x[10], x[14]); QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]); QR(x[1], x[6], x[11], x[12]); QR(x[2], x[7], x[8], x[13]); QR(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; ++i) {
        const uint32_t v = x[i] + in[i];
        out[4 * i] = (uint8_t)v; out[4 * i + 1] = (uint8_t)(v >> 8); out[4 * i + 2] = (uint8_t)(v >> 16); out[4 * i + 3] = (uint8_t)(v >> 24);
    }
}

/* Whether byte p is left as it is. */
__attribute__((unused)) static int clear_byte(const ContentCrypt *c, uint64_t p) {
    return p < c->prefix || (p >= c->tail_at && p - c->tail_at < c->tail_len);
}

void runtime_content_crypt_apply(const ContentCrypt *c, uint64_t offset, void *buffer, size_t size) {
#ifdef HAVE_CONTENT_KEY
    if (!c || !c->on || !size) return;
    make_key();
    uint8_t *b = buffer;
    uint64_t p = offset;
    const uint64_t end = offset + size;
    uint8_t stream[64];
    while (p < end) {
        const uint64_t block = p / 64, block_end = (block + 1) * 64 < end ? (block + 1) * 64 : end;
        /* a block inside a clear range needs no key stream */
        if (clear_byte(c, p) && clear_byte(c, block_end - 1) &&
            (block_end <= c->prefix || (p >= c->tail_at && block_end - c->tail_at <= c->tail_len))) {
            p = block_end;
            continue;
        }
        chacha_block(key_words, (uint32_t)block, c->nonce, stream);
        for (; p < block_end; ++p)
            if (!clear_byte(c, p)) b[p - offset] ^= stream[p % 64];
    }
#else
    (void)c; (void)offset; (void)buffer; (void)size;
#endif
}
