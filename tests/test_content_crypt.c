/* The port's encrypted content (src/runtime_content_crypt.c): the RFC 7539 stream, the trailer, the clear ranges
 * and reads at any offset. Built with -DOCTR_CONTENT_KEY -Itests/content_key (the RFC's example key).
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#include <sys/types.h>
/* compat_win.c's positioned read, as the runtime has it */
ssize_t compat_pread(int fd, void *buffer, size_t size, long long offset) {
    if (_lseeki64(fd, offset, SEEK_SET) < 0) return -1;
    return _read(fd, buffer, (unsigned)size);
}
#endif
#include "../src/runtime_content_crypt.c"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i)); }
static void put64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (8 * i)); }
static void make_trailer(uint8_t *t, const uint8_t nonce[12], uint32_t prefix, uint64_t tail_at, uint32_t tail_len, uint64_t size) {
    memset(t, 0, CONTENT_CRYPT_TRAILER);
    memcpy(t, "OCTRENC1", 8);
    memcpy(t + 8, nonce, 12);
    put32(t + 20, prefix); put64(t + 24, tail_at); put32(t + 32, tail_len); put64(t + 40, size);
}

int main(void) {
    CHECK(runtime_content_crypt_key_ready());
    /* RFC 7539 2.4.2: key 00..1f, nonce 00 00 00 00 00 00 00 4a 00 00 00 00, the first block's counter 1 (a
     * file's bytes 64..127 here), "Ladies and Gentlemen of the class of '99: ..." -> 6e 2e 35 9a ... */
    static const char text[] = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
    static const uint8_t expect[16] = {0x6e, 0x2e, 0x35, 0x9a, 0x25, 0x68, 0xf9, 0x80, 0x41, 0xba, 0x07, 0x28, 0xdd, 0x0d, 0x69, 0x81};
    static const uint8_t expect_end[4] = {0x5e, 0x42, 0x87, 0x4d};   /* its last 4 bytes (114 in all) */
    const size_t n = strlen(text);
    CHECK(n == 114);
    ContentCrypt c = {.on = 1, .size = 4096};
    const uint8_t nonce[12] = {0, 0, 0, 0, 0, 0, 0, 0x4a, 0, 0, 0, 0};
    memcpy(c.nonce, nonce, 12);
    uint8_t buf[256];
    memcpy(buf, text, n);
    runtime_content_crypt_apply(&c, 64, buf, n);
    CHECK(!memcmp(buf, expect, 16));
    CHECK(!memcmp(buf + n - 4, expect_end, 4));
    runtime_content_crypt_apply(&c, 64, buf, n);
    CHECK(!memcmp(buf, text, n));

    /* A file: 1000 bytes, [0, 100) and [700, 750) clear. Encrypted whole, then read back in pieces. */
    uint8_t plain[1000], file[1000];
    for (int i = 0; i < 1000; ++i) plain[i] = (uint8_t)(i * 7 + 3);
    ContentCrypt f = {.on = 1, .prefix = 100, .tail_at = 700, .tail_len = 50, .size = 1000};
    memcpy(f.nonce, "abcdefghijkl", 12);
    memcpy(file, plain, sizeof file);
    runtime_content_crypt_apply(&f, 0, file, sizeof file);
    CHECK(!memcmp(file, plain, 100));
    CHECK(!memcmp(file + 700, plain + 700, 50));
    CHECK(memcmp(file + 100, plain + 100, 600));
    CHECK(memcmp(file + 750, plain + 750, 250));
    int pieces_ok = 1;
    for (int at = 0; at < 1000; at += 37) {
        const int len = at + 123 <= 1000 ? 123 : 1000 - at;
        uint8_t piece[123];
        memcpy(piece, file + at, (size_t)len);
        runtime_content_crypt_apply(&f, (uint64_t)at, piece, (size_t)len);
        if (memcmp(piece, plain + at, (size_t)len)) pieces_ok = 0;
    }
    CHECK(pieces_ok);

    /* The trailer. */
    uint8_t t[CONTENT_CRYPT_TRAILER];
    ContentCrypt got;
    make_trailer(t, (const uint8_t *)"abcdefghijkl", 100, 700, 50, 1000);
    CHECK(runtime_content_crypt_parse(t, 1000 + CONTENT_CRYPT_TRAILER, &got) == 1);
    CHECK(got.on && got.prefix == 100 && got.tail_at == 700 && got.tail_len == 50 && got.size == 1000);
    CHECK(!memcmp(got.nonce, "abcdefghijkl", 12));
    CHECK(runtime_content_crypt_parse(t, 1000 + CONTENT_CRYPT_TRAILER + 1, &got) == -1);   /* size differs */
    make_trailer(t, nonce, 100, 990, 50, 1000);
    CHECK(runtime_content_crypt_parse(t, 1000 + CONTENT_CRYPT_TRAILER, &got) == -1);       /* names past the end */
    memcpy(t, "NOTMAGIC", 8);
    CHECK(runtime_content_crypt_parse(t, 1000 + CONTENT_CRYPT_TRAILER, &got) == 0);

    /* An open file with a trailer. */
    char name[64];
    snprintf(name, sizeof name, "content-crypt-test-%d.bin", (int)getpid());
    FILE *w = fopen(name, "wb");
    make_trailer(t, (const uint8_t *)"abcdefghijkl", 100, 700, 50, 1000);
    fwrite(file, 1, sizeof file, w);
    fwrite(t, 1, sizeof t, w);
    fclose(w);
    FILE *r = fopen(name, "rb");
    CHECK(runtime_content_crypt_probe(fileno(r), 1000 + CONTENT_CRYPT_TRAILER, &got) == 1 && got.size == 1000);
    fclose(r);
    remove(name);

    printf(failures ? "content crypt FAIL\n" : "content crypt PASS\n");
    return failures != 0;
}
