/**
 * @file sha256.h
 * @brief Helios OS — SHA-256 hash (FIPS 180-4).
 */

#ifndef HELIOS_CRYPTO_SHA256_H
#define HELIOS_CRYPTO_SHA256_H

#include <helios/types.h>

#define SHA256_BLOCK_SIZE   64
#define SHA256_DIGEST_SIZE  32

typedef struct {
    uint32_t state[8];
    uint64_t count;       /* Total bytes processed */
    uint8_t  buf[64];     /* Partial block buffer  */
} sha256_context_t;

/**
 * @brief Initialize a SHA-256 context.
 */
void sha256_init(sha256_context_t *ctx);

/**
 * @brief Feed data into the SHA-256 context.
 */
void sha256_update(sha256_context_t *ctx, const void *data, size_t len);

/**
 * @brief Finalize and produce the 32-byte digest.
 */
void sha256_final(sha256_context_t *ctx, uint8_t digest[32]);

/**
 * @brief One-shot SHA-256: hash data and produce digest.
 */
void sha256(const void *data, size_t len, uint8_t digest[32]);

#endif /* HELIOS_CRYPTO_SHA256_H */
