/**
 * @file hmac.c
 * @brief Helios OS — HMAC-SHA256 (RFC 2104).
 *
 * Standard HMAC construction using SHA-256:
 *   HMAC(K, m) = H((K' ⊕ opad) || H((K' ⊕ ipad) || m))
 *
 * Where K' is the key (hashed if longer than 64 bytes, zero-padded if shorter),
 * ipad = 0x36 repeated, opad = 0x5C repeated.
 */

#include <helios/crypto/hmac.h>
#include <helios/crypto/sha256.h>

#ifndef UNIT_TEST
extern void *memcpy(void *dest, const void *src, size_t n);
extern void *memset(void *dest, int val, size_t n);
#else
#include <string.h>
#endif

#define HMAC_IPAD 0x36
#define HMAC_OPAD 0x5C

void hmac_sha256_init(hmac_sha256_context_t *ctx,
                      const uint8_t *key, size_t key_len) {
    uint8_t k_prime[SHA256_BLOCK_SIZE];

    /* If key is longer than block size, hash it first */
    if (key_len > SHA256_BLOCK_SIZE) {
        sha256(key, key_len, k_prime);
        memset(k_prime + SHA256_DIGEST_SIZE, 0,
               SHA256_BLOCK_SIZE - SHA256_DIGEST_SIZE);
    } else {
        memcpy(k_prime, key, key_len);
        memset(k_prime + key_len, 0, SHA256_BLOCK_SIZE - key_len);
    }

    /* Inner hash: SHA256((K' ⊕ ipad) || ...) */
    uint8_t ipad_key[SHA256_BLOCK_SIZE];
    for (size_t i = 0; i < SHA256_BLOCK_SIZE; i++) {
        ipad_key[i] = k_prime[i] ^ HMAC_IPAD;
    }
    sha256_init(&ctx->inner);
    sha256_update(&ctx->inner, ipad_key, SHA256_BLOCK_SIZE);

    /* Outer hash: SHA256((K' ⊕ opad) || ...) */
    uint8_t opad_key[SHA256_BLOCK_SIZE];
    for (size_t i = 0; i < SHA256_BLOCK_SIZE; i++) {
        opad_key[i] = k_prime[i] ^ HMAC_OPAD;
    }
    sha256_init(&ctx->outer);
    sha256_update(&ctx->outer, opad_key, SHA256_BLOCK_SIZE);
}

void hmac_sha256_update(hmac_sha256_context_t *ctx,
                        const void *data, size_t len) {
    sha256_update(&ctx->inner, data, len);
}

void hmac_sha256_final(hmac_sha256_context_t *ctx, uint8_t mac[32]) {
    uint8_t inner_digest[SHA256_DIGEST_SIZE];

    /* Finalize inner hash */
    sha256_final(&ctx->inner, inner_digest);

    /* Feed inner digest into outer hash and finalize */
    sha256_update(&ctx->outer, inner_digest, SHA256_DIGEST_SIZE);
    sha256_final(&ctx->outer, mac);
}
