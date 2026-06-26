/**
 * @file capability.h
 * @brief Helios OS — Capability token system (HMAC-SHA256 signed).
 *
 * Capability tokens are unforgeable descriptors granting access to a
 * specific memory region with specific permissions. Each token is
 * HMAC-signed with a per-boot kernel secret key.
 */

#ifndef HELIOS_CAPABILITY_H
#define HELIOS_CAPABILITY_H

#include <helios/types.h>

/* ═══════════════════════════════════════════════════════════════════════════
 */
/*  Permission bits                                                           */
/* ═══════════════════════════════════════════════════════════════════════════
 */

#define CAP_PERM_READ (1u << 0)
#define CAP_PERM_WRITE (1u << 1)
#define CAP_PERM_EXEC (1u << 2)
#define CAP_PERM_SHARE (1u << 3)
#define CAP_PERM_DERIVE (1u << 4)
#define CAP_PERM_DMA (1u << 5)
#define CAP_PERM_NOCACHE (1u << 6)
#define CAP_PERM_PIN (1u << 7)

/* ═══════════════════════════════════════════════════════════════════════════
 */
/*  Capability token structure                                                */
/* ═══════════════════════════════════════════════════════════════════════════
 */

typedef struct {
  uint64_t base;        /**< Virtual base address              */
  uint64_t length;      /**< Region size in bytes              */
  uint32_t permissions; /**< CAP_PERM_* bitmask                */
  uint32_t owner_id;    /**< Owning micro-program ID           */
  uint64_t nonce;       /**< Unique random value               */
  uint8_t hmac[16];     /**< Truncated HMAC-SHA256 (first 16B) */
} PACKED cap_token_t;

_Static_assert(sizeof(cap_token_t) == 48, "cap_token_t must be 48 bytes");

/* ═══════════════════════════════════════════════════════════════════════════
 */
/*  Public API                                                                */
/* ═══════════════════════════════════════════════════════════════════════════
 */

/**
 * @brief Initialize the capability manager.
 *
 * Derives the HMAC key from RDRAND + TSC entropy via SHA-256.
 * Must be called after slab_init().
 */
void cap_manager_init(void);

/**
 * @brief Create a new capability token.
 *
 * Generates a random nonce and computes the truncated HMAC-SHA256
 * over {base, length, permissions, owner_id, nonce}.
 */
cap_token_t cap_create(uint64_t base, uint64_t length, uint32_t perms,
                       uint32_t owner_id);

/**
 * @brief Validate a capability token's HMAC.
 *
 * Recomputes the HMAC and compares with constant-time XOR.
 * @return true if valid, false if tampered.
 */
bool cap_validate(const cap_token_t *token);

/**
 * @brief Derive a sub-capability from a parent.
 *
 * Enforces: new bounds ⊆ parent bounds, new_perms ⊆ parent perms.
 * Returns zeroed token on violation.
 */
cap_token_t cap_derive(const cap_token_t *parent, uint64_t new_base,
                       uint64_t new_length, uint32_t new_perms);

/**
 * @brief Revoke a single capability token.
 *
 * Records the token's nonce in the revocation table.  Subsequent calls to
 * cap_validate() for any token with this nonce will return false.
 */
void cap_revoke(const cap_token_t *token);

/**
 * @brief Revoke all capability tokens belonging to an owner.
 *
 * Scans the revocation table for every nonce registered under owner_id
 * and marks them all revoked.  Used on micro-program teardown.
 */
void cap_revoke_all(uint32_t owner_id);

#endif /* HELIOS_CAPABILITY_H */
