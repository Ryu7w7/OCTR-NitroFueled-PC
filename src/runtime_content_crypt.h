/* The port's content files kept encrypted on disk (runtime_content_crypt.c, tools/content_crypt.py).
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define CONTENT_CRYPT_TRAILER 48

typedef struct {
    int on;
    uint8_t nonce[12];
    uint32_t prefix;            /* bytes [0, prefix) are not encrypted (an archive's index) */
    uint64_t tail_at;           /* nor [tail_at, tail_at + tail_len) (an archive's names) */
    uint32_t tail_len;
    uint64_t size;              /* the file's own size (the trailer after it) */
} ContentCrypt;

/* The trailer (the file's last 48 bytes): 1 an encrypted file (c filled), 0 not one, -1 a damaged one. */
int runtime_content_crypt_parse(const uint8_t trailer[CONTENT_CRYPT_TRAILER], uint64_t file_size, ContentCrypt *c);
/* An open file: 1 encrypted and readable (c filled), 0 not encrypted, -1 encrypted but this build cannot
 * read it (no content key) or damaged. */
int runtime_content_crypt_probe(int fd, uint64_t file_size, ContentCrypt *c);
/* The bytes read at offset (of the file's own bytes) decrypted in place. */
void runtime_content_crypt_apply(const ContentCrypt *c, uint64_t offset, void *buffer, size_t size);
/* Whether this build has the content key (official builds: gpu/CMakeLists.txt SACREDFIRE_CONTENT_KEY_FILE). */
int runtime_content_crypt_key_ready(void);
