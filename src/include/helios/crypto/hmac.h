/**
 * @file hmac.h
 * @brief Helios OS — HMAC-SHA256 (RFC 2104).
 */

#ifndef HELIOS_CRYPTO_HMAC_H
#define HELIOS_CRYPTO_HMAC_H

#include <helios/types.h>
#include <helios/crypto/sha256.h>

typedef struct {
    sha256_context_t inner;
    sha256_context_t outer;
} hmac_sha256_context_t;

/**
 * @brief Initialize an HMAC-SHA256 context with a key.
 */
void hmac_sha256_init(hmac_sha256_context_t *ctx,
                      const uint8_t *key, size_t key_len);

/**
 * @brief Feed data into the HMAC-SHA256 context.
 */
void hmac_sha256_update(hmac_sha256_context_t *ctx,
                        const void *data, size_t len);

/**
 * @brief Finalize and produce the 32-byte HMAC.
 */
void hmac_sha256_final(hmac_sha256_context_t *ctx, uint8_t mac[32]);

#endif /* HELIOS_CRYPTO_HMAC_H */
