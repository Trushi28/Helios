/**
 * @file sha256.c
 * @brief Helios OS — SHA-256 implementation (FIPS 180-4).
 *
 * Pure software, no hardware acceleration. Suitable for freestanding
 * kernel use — no libc dependency.
 */

#include <helios/crypto/sha256.h>

#ifndef UNIT_TEST
extern void *memcpy(void *dest, const void *src, size_t n);
extern void *memset(void *dest, int val, size_t n);
#else
#include <string.h>
#endif

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  SHA-256 constants (FIPS 180-4 §4.2.2)                                    */
/* ═══════════════════════════════════════════════════════════════════════════ */

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Bit manipulation helpers                                                  */
/* ═══════════════════════════════════════════════════════════════════════════ */

static inline uint32_t rotr32(uint32_t x, unsigned n) {
    return (x >> n) | (x << (32 - n));
}

static inline uint32_t ch(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (~x & z);
}

static inline uint32_t maj(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (x & z) ^ (y & z);
}

static inline uint32_t sigma0(uint32_t x) {
    return rotr32(x, 2) ^ rotr32(x, 13) ^ rotr32(x, 22);
}

static inline uint32_t sigma1(uint32_t x) {
    return rotr32(x, 6) ^ rotr32(x, 11) ^ rotr32(x, 25);
}

static inline uint32_t gamma0(uint32_t x) {
    return rotr32(x, 7) ^ rotr32(x, 18) ^ (x >> 3);
}

static inline uint32_t gamma1(uint32_t x) {
    return rotr32(x, 17) ^ rotr32(x, 19) ^ (x >> 10);
}

/* Big-endian load/store */
static inline uint32_t be32_load(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static inline void be32_store(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static inline void be64_store(uint8_t *p, uint64_t v) {
    p[0] = (uint8_t)(v >> 56);
    p[1] = (uint8_t)(v >> 48);
    p[2] = (uint8_t)(v >> 40);
    p[3] = (uint8_t)(v >> 32);
    p[4] = (uint8_t)(v >> 24);
    p[5] = (uint8_t)(v >> 16);
    p[6] = (uint8_t)(v >> 8);
    p[7] = (uint8_t)v;
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  SHA-256 transform (one 64-byte block)                                     */
/* ═══════════════════════════════════════════════════════════════════════════ */

static void sha256_transform(sha256_context_t *ctx, const uint8_t block[64]) {
    uint32_t W[64];
    uint32_t a, b, c, d, e, f, g, h;

    /* Prepare message schedule */
    for (int i = 0; i < 16; i++) {
        W[i] = be32_load(block + i * 4);
    }
    for (int i = 16; i < 64; i++) {
        W[i] = gamma1(W[i-2]) + W[i-7] + gamma0(W[i-15]) + W[i-16];
    }

    /* Initialize working variables */
    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];

    /* 64 rounds */
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + sigma1(e) + ch(e, f, g) + K[i] + W[i];
        uint32_t t2 = sigma0(a) + maj(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    /* Update state */
    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Public API                                                                */
/* ═══════════════════════════════════════════════════════════════════════════ */

void sha256_init(sha256_context_t *ctx) {
    /* FIPS 180-4 §5.3.3: initial hash values */
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
    ctx->count = 0;
}

void sha256_update(sha256_context_t *ctx, const void *data, size_t len) {
    const uint8_t *src = (const uint8_t *)data;
    size_t buffered = (size_t)(ctx->count % SHA256_BLOCK_SIZE);
    ctx->count += len;

    /* If we have buffered data and can complete a block */
    if (buffered > 0) {
        size_t fill = SHA256_BLOCK_SIZE - buffered;
        if (len >= fill) {
            memcpy(ctx->buf + buffered, src, fill);
            sha256_transform(ctx, ctx->buf);
            src += fill;
            len -= fill;
            buffered = 0;
        } else {
            memcpy(ctx->buf + buffered, src, len);
            return;
        }
    }

    /* Process full blocks */
    while (len >= SHA256_BLOCK_SIZE) {
        sha256_transform(ctx, src);
        src += SHA256_BLOCK_SIZE;
        len -= SHA256_BLOCK_SIZE;
    }

    /* Buffer remaining bytes */
    if (len > 0) {
        memcpy(ctx->buf, src, len);
    }
}

void sha256_final(sha256_context_t *ctx, uint8_t digest[32]) {
    uint64_t bit_count = ctx->count * 8;
    size_t buffered = (size_t)(ctx->count % SHA256_BLOCK_SIZE);

    /* Append 0x80 padding byte */
    ctx->buf[buffered++] = 0x80;

    /* If not enough room for the 8-byte length, pad this block and process */
    if (buffered > 56) {
        memset(ctx->buf + buffered, 0, SHA256_BLOCK_SIZE - buffered);
        sha256_transform(ctx, ctx->buf);
        buffered = 0;
    }

    /* Pad to 56 bytes and append 64-bit big-endian bit count */
    memset(ctx->buf + buffered, 0, 56 - buffered);
    be64_store(ctx->buf + 56, bit_count);
    sha256_transform(ctx, ctx->buf);

    /* Produce big-endian digest */
    for (int i = 0; i < 8; i++) {
        be32_store(digest + i * 4, ctx->state[i]);
    }
}

void sha256(const void *data, size_t len, uint8_t digest[32]) {
    sha256_context_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, digest);
}
