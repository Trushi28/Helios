/**
 * @file capability.c
 * @brief Helios OS — Capability token manager.
 *
 * Creates, validates, and derives HMAC-signed capability tokens.
 * The HMAC key is derived from RDRAND + TSC entropy at boot and
 * never leaves kernel memory.
 *
 * Revocation uses an open-addressing hash table keyed by nonce.
 * Each slot also stores the owner_id to support cap_revoke_all().
 * Tombstones (REVOC_DEAD) allow deletion without breaking probe chains.
 */

#include <helios/capability.h>
#include <helios/crypto/hmac.h>
#include <helios/crypto/sha256.h>
#include <helios/spinlock.h>
#include <helios/types.h>

/* Forward declarations */
extern void serial_printf(const char *fmt, ...);
extern void serial_puts(const char *s);
extern NORETURN void panic(const char *msg);
extern void *memcpy(void *dest, const void *src, size_t n);
extern void *memset(void *dest, int val, size_t n);
extern bool random_read(void *buf, size_t n_bytes);

/* ═══════════════════════════════════════════════════════════════════════════
 */
/*  Global HMAC key — regenerated each boot                                   */
/* ═══════════════════════════════════════════════════════════════════════════
 */

static uint8_t g_cap_hmac_key[32];
static bool g_cap_initialized = false;

/* ═══════════════════════════════════════════════════════════════════════════
 */
/*  Revocation table                                                          */
/* ═══════════════════════════════════════════════════════════════════════════
 */

#define REVOC_CAPACITY 4096u /* must be power of 2 */
/* NOTE: Phase 3 will replace this static table with a dynamically
 * growing hash table backed by slab-allocated pages. The 4096-entry
 * limit is sufficient for Phase 2 but must not be carried forward
 * into production. See docs/17-ROADMAP.md, Section 3.x. */
#define REVOC_EMPTY 0u       /* slot is unused                */
#define REVOC_DEAD 1u        /* tombstone (deleted entry)     */
#define REVOC_LIVE 2u        /* active revoked nonce          */

typedef struct {
  uint64_t nonce;
  uint32_t owner_id;
  uint32_t state; /* REVOC_EMPTY / REVOC_DEAD / REVOC_LIVE */
} revoc_entry_t;

static revoc_entry_t g_revoc_table[REVOC_CAPACITY];
static uint32_t g_revoc_count; /* live entries */
static spinlock_t g_revoc_lock;

/* Hash function: mix the nonce bits */
static inline uint32_t revoc_hash(uint64_t nonce) {
  nonce ^= nonce >> 33;
  nonce *= 0xff51afd7ed558ccdULL;
  nonce ^= nonce >> 33;
  return (uint32_t)(nonce & (REVOC_CAPACITY - 1));
}

/**
 * @brief Check whether a nonce is in the revocation table.
 * @return true if revoked, false if not found.
 */
static bool revoc_contains(uint64_t nonce) {
  uint32_t idx = revoc_hash(nonce);
  for (uint32_t i = 0; i < REVOC_CAPACITY; i++) {
    uint32_t slot = (idx + i) & (REVOC_CAPACITY - 1);
    if (g_revoc_table[slot].state == REVOC_EMPTY)
      return false; /* hit an empty slot — not present */
    if (g_revoc_table[slot].state == REVOC_LIVE &&
        g_revoc_table[slot].nonce == nonce)
      return true;
    /* REVOC_DEAD: skip (tombstone) and keep probing */
  }
  return false;
}

/**
 * @brief Insert a nonce into the revocation table.
 */
static void revoc_insert(uint64_t nonce, uint32_t owner_id) {
  if (g_revoc_count >= (REVOC_CAPACITY * 3 / 4)) {
    /* Table is 75% full — this is a hard limit for Phase 2.
     * In practice, 4096 entries is far more than enough for the
     * number of capabilities that exist before Phase 3 adds
     * a dynamically growing table. */
    panic("CAP: revocation table full");
  }

  uint32_t idx = revoc_hash(nonce);
  for (uint32_t i = 0; i < REVOC_CAPACITY; i++) {
    uint32_t slot = (idx + i) & (REVOC_CAPACITY - 1);
    if (g_revoc_table[slot].state == REVOC_EMPTY ||
        g_revoc_table[slot].state == REVOC_DEAD) {
      g_revoc_table[slot].nonce = nonce;
      g_revoc_table[slot].owner_id = owner_id;
      g_revoc_table[slot].state = REVOC_LIVE;
      g_revoc_count++;
      return;
    }
  }
  panic("CAP: revocation table insert failed");
}

/* ═══════════════════════════════════════════════════════════════════════════
 */
/*  HMAC computation over token fields                                        */
/* ═══════════════════════════════════════════════════════════════════════════
 */

/**
 * @brief Compute HMAC-SHA256 over the token's authenticated fields.
 *
 * Authenticated fields: {base, length, permissions, owner_id, nonce}
 * = 8 + 8 + 4 + 4 + 8 = 32 bytes
 */
static void cap_compute_hmac_internal(const cap_token_t *token,
                                      uint8_t out_mac[32]) {
  hmac_sha256_context_t ctx;
  hmac_sha256_init(&ctx, g_cap_hmac_key, sizeof(g_cap_hmac_key));

  /* Feed authenticated fields in order */
  hmac_sha256_update(&ctx, &token->base, sizeof(token->base));
  hmac_sha256_update(&ctx, &token->length, sizeof(token->length));
  hmac_sha256_update(&ctx, &token->permissions, sizeof(token->permissions));
  hmac_sha256_update(&ctx, &token->owner_id, sizeof(token->owner_id));
  hmac_sha256_update(&ctx, &token->nonce, sizeof(token->nonce));

  hmac_sha256_final(&ctx, out_mac);
}

/* ═══════════════════════════════════════════════════════════════════════════
 */
/*  Public API                                                                */
/* ═══════════════════════════════════════════════════════════════════════════
 */

void cap_manager_init(void) {
  /* Read 32 bytes of entropy from RDRAND (or TSC-LCG fallback) */
  uint8_t entropy[32];
  random_read(entropy, sizeof(entropy));

  /* Mix with current TSC value */
  uint64_t tsc = rdtsc();
  uint8_t seed_material[40]; /* 32 bytes entropy + 8 bytes TSC */
  memcpy(seed_material, entropy, 32);
  memcpy(seed_material + 32, &tsc, 8);

  /* Derive key: SHA-256(entropy || tsc) → g_cap_hmac_key */
  sha256(seed_material, sizeof(seed_material), g_cap_hmac_key);

  /* Initialize revocation table */
  memset(g_revoc_table, 0, sizeof(g_revoc_table));
  g_revoc_count = 0;
  g_revoc_lock = (spinlock_t)SPINLOCK_INIT;

  g_cap_initialized = true;
  serial_puts("  CAP: HMAC key derived from RDRAND+TSC entropy\n");
}

cap_token_t cap_create(uint64_t base, uint64_t length, uint32_t perms,
                       uint32_t owner_id) {
  cap_token_t token;

  if (!g_cap_initialized) {
    panic("cap_create: capability manager not initialized");
  }

  token.base = base;
  token.length = length;
  token.permissions = perms;
  token.owner_id = owner_id;

  /* Generate random nonce */
  random_read(&token.nonce, sizeof(token.nonce));

  /* Compute truncated HMAC-SHA256 (first 16 bytes) */
  uint8_t full_mac[32];
  cap_compute_hmac_internal(&token, full_mac);
  memcpy(token.hmac, full_mac, 16);

  return token;
}

bool cap_validate(const cap_token_t *token) {
  if (!g_cap_initialized) {
    return false;
  }

  /* 1. HMAC check — detects forgery and tampering */
  uint8_t full_mac[32];
  cap_compute_hmac_internal(token, full_mac);

  uint8_t diff = 0;
  for (int i = 0; i < 16; i++) {
    diff |= token->hmac[i] ^ full_mac[i];
  }
  if (diff != 0)
    return false;

  /* 2. Revocation check — O(1) average hash table lookup */
  spinlock_lock(&g_revoc_lock);
  bool revoked = revoc_contains(token->nonce);
  spinlock_unlock(&g_revoc_lock);

  return !revoked;
}

cap_token_t cap_derive(const cap_token_t *parent, uint64_t new_base,
                       uint64_t new_length, uint32_t new_perms) {
  cap_token_t zero;
  memset(&zero, 0, sizeof(zero));

  /* Validate the parent token first */
  if (!cap_validate(parent)) {
    return zero;
  }

  /* Parent must have DERIVE permission */
  if (!(parent->permissions & CAP_PERM_DERIVE)) {
    return zero;
  }

  /* New bounds must be within parent bounds */
  if (new_base < parent->base) {
    return zero;
  }
  if (new_base + new_length > parent->base + parent->length) {
    return zero;
  }

  /* New permissions must be a subset of parent permissions */
  if ((new_perms & ~parent->permissions) != 0) {
    return zero;
  }

  /* Create the derived token */
  return cap_create(new_base, new_length, new_perms, parent->owner_id);
}

void cap_revoke(const cap_token_t *token) {
  if (!g_cap_initialized)
    return;

  /* Only revoke tokens that currently pass HMAC validation */
  if (!cap_validate(token))
    return;

  spinlock_lock(&g_revoc_lock);
  revoc_insert(token->nonce, token->owner_id);
  spinlock_unlock(&g_revoc_lock);
}

void cap_revoke_all(uint32_t owner_id) {
  if (!g_cap_initialized)
    return;

  /* Linear scan: mark every live entry with matching owner_id as a
   * tombstone.  O(REVOC_CAPACITY) = O(4096) — acceptable at Phase 2
   * teardown frequency (called once per dying micro-program). */
  spinlock_lock(&g_revoc_lock);
  for (uint32_t i = 0; i < REVOC_CAPACITY; i++) {
    if (g_revoc_table[i].state == REVOC_LIVE &&
        g_revoc_table[i].owner_id == owner_id) {
      g_revoc_table[i].state = REVOC_DEAD;
      g_revoc_count--;
    }
  }
  spinlock_unlock(&g_revoc_lock);
}
