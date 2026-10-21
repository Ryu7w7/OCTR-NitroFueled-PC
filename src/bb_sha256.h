/* SHA-256 (FIPS 180-4) and HMAC-SHA256 (RFC 2104), header-only: the ISA rewrite checks its
 * table with it, runtime_http.c signs RyuNET requests with it. */
#pragma once
#include <stdint.h>
#include <string.h>

static inline uint32_t bb_ror32(uint32_t x, int n) { return x >> n | x << (32 - n); }

static inline void bb_sha256_block(uint32_t h[8], const unsigned char *block) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64], v[8];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)block[4 * i] << 24 | (uint32_t)block[4 * i + 1] << 16 | (uint32_t)block[4 * i + 2] << 8 |
               block[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = bb_ror32(w[i - 15], 7) ^ bb_ror32(w[i - 15], 18) ^ w[i - 15] >> 3;
        uint32_t s1 = bb_ror32(w[i - 2], 17) ^ bb_ror32(w[i - 2], 19) ^ w[i - 2] >> 10;
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    memcpy(v, h, sizeof v);
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = v[7] + (bb_ror32(v[4], 6) ^ bb_ror32(v[4], 11) ^ bb_ror32(v[4], 25)) + ((v[4] & v[5]) ^ (~v[4] & v[6])) +
                      k[i] + w[i];
        uint32_t t2 = (bb_ror32(v[0], 2) ^ bb_ror32(v[0], 13) ^ bb_ror32(v[0], 22)) + ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
        memmove(v + 1, v, 7 * sizeof *v);
        v[4] += t1;
        v[0] = t1 + t2;
    }
    for (int i = 0; i < 8; i++) h[i] += v[i];
}

static inline void bb_sha256(const unsigned char *data, uint64_t size, uint8_t out[32]) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint64_t done = 0;
    for (; size - done >= 64; done += 64) bb_sha256_block(h, data + done);
    unsigned char tail[128] = {0};
    size_t rest = (size_t)(size - done), tail_size = rest < 56 ? 64 : 128;
    memcpy(tail, data + done, rest);
    tail[rest] = 0x80;
    for (int i = 0; i < 8; i++) tail[tail_size - 1 - i] = (unsigned char)(size * 8 >> 8 * i);
    for (size_t at = 0; at < tail_size; at += 64) bb_sha256_block(h, tail + at);
    for (int i = 0; i < 32; i++) out[i] = (uint8_t)(h[i / 4] >> (24 - 8 * (i % 4)));
}

static inline void bb_hmac_sha256(const unsigned char *key, size_t key_size, const unsigned char *msg, size_t msg_size,
                                  uint8_t out[32]) {
    unsigned char k[64] = {0}, pad[64 + 512];
    if (key_size > 64) bb_sha256(key, key_size, k);
    else memcpy(k, key, key_size);
    uint8_t inner[32];
    if (msg_size > sizeof pad - 64) msg_size = sizeof pad - 64; /* callers sign short messages */
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    memcpy(pad + 64, msg, msg_size);
    bb_sha256(pad, 64 + msg_size, inner);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    memcpy(pad + 64, inner, 32);
    bb_sha256(pad, 64 + 32, out);
}
