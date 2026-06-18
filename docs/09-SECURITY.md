# 09 — Capability System & Secure Boot Chain

> **Subsystem:** Security  
> **Owner:** Security team  
> **Dependencies:** UEFI Secure Boot, TPM 2.0, capability manager, SHA-256  
> **Related:** [01-BOOT.md](./01-BOOT.md), [02-MEMORY.md](./02-MEMORY.md), [08-IPC.md](./08-IPC.md)

---

## 1. Security Model Overview

Helios replaces the traditional Unix DAC (Discretionary Access Control) and Linux LSM models with a **pure capability-based security architecture**. There are no user IDs, no file permissions, no root/sudo escalation. Every access to every resource — memory, devices, objects, services — requires presenting a valid, cryptographically signed capability token.

```
Traditional Security Stack          Helios Security Stack
─────────────────────────          ─────────────────────
  Root / sudo escalation              (none)
  SELinux / AppArmor policies          (none)
  File permission bits (rwx)           (none)
  User IDs / Group IDs                 (none)
  Process isolation (per-AS)           (none)
                                      
  ↓ all replaced by ↓                 Capability Tokens
                                      + IOMMU hardware enforcement
                                      + Secure boot chain
                                      + Object signing
```

### 1.1 Principle of Least Authority (POLA)

Every micro-program starts with **zero capabilities**. It can only access resources explicitly granted to it by the kernel or by another micro-program that already holds the capability. Capabilities can only be narrowed (derived with fewer permissions), never widened.

---

## 2. Capability Token Deep Dive

### 2.1 Token Format (Recap from 02-MEMORY)

```c
typedef struct {
    uint64_t base;          // Virtual base address
    uint64_t length;        // Region size
    uint32_t permissions;   // Bitmask
    uint32_t owner_id;      // Owning micro-program
    uint64_t nonce;         // Unique random value
    uint8_t  hmac[16];      // Truncated HMAC-SHA256
} __attribute__((packed)) cap_token_t;
```

### 2.2 HMAC Computation

The HMAC is computed over all fields except the HMAC itself, using a kernel-private secret key:

```c
// Kernel-private HMAC key — generated at boot from hardware RNG
static uint8_t g_cap_hmac_key[32];

void cap_compute_hmac(cap_token_t *token) {
    // HMAC-SHA256 over {base || length || permissions || owner_id || nonce}
    hmac_sha256_context ctx;
    hmac_sha256_init(&ctx, g_cap_hmac_key, 32);
    hmac_sha256_update(&ctx, &token->base, 8);
    hmac_sha256_update(&ctx, &token->length, 8);
    hmac_sha256_update(&ctx, &token->permissions, 4);
    hmac_sha256_update(&ctx, &token->owner_id, 4);
    hmac_sha256_update(&ctx, &token->nonce, 8);

    uint8_t full_hmac[32];
    hmac_sha256_final(&ctx, full_hmac);

    // Truncate to 128 bits (sufficient for anti-forgery)
    memcpy(token->hmac, full_hmac, 16);
}

bool cap_verify_hmac(const cap_token_t *token) {
    cap_token_t check = *token;
    cap_compute_hmac(&check);
    return crypto_memcmp(check.hmac, token->hmac, 16) == 0;
}
```

### 2.3 HMAC Key Derivation

The HMAC key is derived at boot from hardware entropy:

```c
void cap_init_hmac_key(void) {
    // 1. Read RDRAND/RDSEED for hardware entropy
    for (int i = 0; i < 32; i += 8) {
        uint64_t rand;
        while (!__builtin_ia32_rdrand64_step(&rand));
        memcpy(&g_cap_hmac_key[i], &rand, 8);
    }

    // 2. Mix with TSC for additional entropy
    uint64_t tsc = rdtsc();
    sha256_context ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, g_cap_hmac_key, 32);
    sha256_update(&ctx, &tsc, 8);
    sha256_final(&ctx, g_cap_hmac_key);

    // 3. Key is never exposed outside the kernel
    // 4. Key changes every boot (capabilities don't persist across reboots)
}
```

### 2.4 Capability Invariants

The following invariants are enforced by the kernel at all times:

1. **No forgery:** A valid HMAC can only be produced by the kernel (holder of the HMAC key)
2. **No amplification:** `cap_derive()` can only produce tokens with equal or fewer permissions and equal or smaller bounds
3. **No replay:** Each token has a unique nonce; revoked nonces are tracked in a revocation table
4. **Monotonic narrowing:** The derivation chain forms a tree rooted at the kernel's omnipotent capability

---

## 3. Revocation

### 3.1 Revocation Table

```c
// Bloom filter + hash set for fast revocation checks
typedef struct {
    // Bloom filter for fast negative lookups (is this nonce NOT revoked?)
    uint64_t        bloom[4096];        // 256 KiB bloom filter (2M bits)
    uint32_t        bloom_hash_count;   // Number of hash functions (k=7)

    // Hash table for definitive lookups (when bloom says "maybe")
    struct hash_table nonce_set;

    uint64_t        revoked_count;
    spinlock_t      lock;
} revocation_table_t;

bool cap_is_revoked(uint64_t nonce) {
    // Fast path: bloom filter says definitely not revoked
    if (!bloom_maybe_contains(&g_revocation.bloom, nonce))
        return false;

    // Slow path: check hash table
    return hash_table_contains(&g_revocation.nonce_set, nonce);
}
```

### 3.2 Cascading Revocation

When a capability is revoked, all capabilities derived from it must also be revoked. The kernel maintains a derivation tree:

```c
typedef struct cap_node {
    uint64_t            nonce;
    uint64_t            parent_nonce;    // 0 for root capabilities
    struct list_head    children;        // Derived capabilities
    struct list_head    sibling;
} cap_node_t;

void cap_revoke_subtree(uint64_t root_nonce) {
    cap_node_t *root = cap_tree_find(root_nonce);
    if (!root) return;

    // BFS traversal of derivation tree
    queue_t queue;
    queue_push(&queue, root);

    while (!queue_empty(&queue)) {
        cap_node_t *node = queue_pop(&queue);
        revocation_table_insert(node->nonce);

        list_for_each_entry(child, &node->children, sibling) {
            queue_push(&queue, child);
        }

        cap_tree_remove(node);
    }
}
```

---

## 4. Secure Boot Chain

### 4.1 Trust Chain

```
┌──────────────────────────────────────────────────┐
│  UEFI Secure Boot (platform firmware)             │
│  └─ Verifies: BOOTX64.EFI signature              │
│     (signed with Helios Secure Boot key)          │
└───────────────────────┬──────────────────────────┘
                        │
                        ▼
┌──────────────────────────────────────────────────┐
│  Helios Bootloader (BOOTX64.EFI)                  │
│  └─ Verifies: KERNEL.BIN signature                │
│     (embedded public key + Ed25519 signature)      │
└───────────────────────┬──────────────────────────┘
                        │
                        ▼
┌──────────────────────────────────────────────────┐
│  Helios Kernel                                    │
│  └─ Verifies: Driver micro-program signatures     │
│  └─ Verifies: Object store integrity (SHA-256)    │
│  └─ Verifies: Base model integrity (SHA-256)      │
└───────────────────────┬──────────────────────────┘
                        │
                        ▼
┌──────────────────────────────────────────────────┐
│  User Micro-Programs                              │
│  └─ Verified by: Object OID (content hash)        │
│  └─ Authorized by: Capability tokens              │
└──────────────────────────────────────────────────┘
```

### 4.2 Kernel Signature Verification

```c
typedef struct {
    uint8_t     magic[8];           // "HELIOSIG"
    uint8_t     public_key[32];     // Ed25519 public key
    uint8_t     signature[64];      // Ed25519 signature over kernel image
    uint64_t    image_size;         // Size of the signed kernel binary
    uint8_t     image_hash[32];     // SHA-256 of kernel binary
} kernel_signature_t;

bool verify_kernel_signature(const void *kernel_image, uint64_t size,
                             const kernel_signature_t *sig) {
    // 1. Verify the image hash
    uint8_t computed_hash[32];
    sha256(kernel_image, size, computed_hash);
    if (memcmp(computed_hash, sig->image_hash, 32) != 0)
        return false;

    // 2. Verify Ed25519 signature
    return ed25519_verify(sig->signature, sig->image_hash, 32,
                          sig->public_key);
}
```

### 4.3 TPM 2.0 Integration (Optional)

If a TPM 2.0 chip is present, Helios extends measurements into PCR banks during boot:

| PCR | Measurement |
|-----|------------|
| PCR 8 | Helios bootloader hash |
| PCR 9 | Kernel image hash |
| PCR 10 | Driver micro-program hashes |
| PCR 11 | Base model hash |
| PCR 12 | Capability HMAC key seed (sealed) |

```c
// Extend a PCR with a measurement
void tpm2_pcr_extend(uint32_t pcr_index, const uint8_t *hash, uint32_t hash_len);

// Seal data to specific PCR values (e.g., HMAC key seed)
int tpm2_seal(uint32_t pcr_mask, const void *data, uint32_t data_len,
              void *sealed_out, uint32_t *sealed_len);

// Unseal data (only succeeds if PCR values match)
int tpm2_unseal(const void *sealed, uint32_t sealed_len,
                void *data_out, uint32_t *data_len);
```

---

## 5. Object Signing

### 5.1 Signed Objects

Critical objects in the graph store (executables, drivers, system configuration) can be signed:

```c
typedef struct {
    object_id_t     oid;            // Content hash of the object
    uint8_t         signer_key[32]; // Ed25519 public key of signer
    uint8_t         signature[64];  // Ed25519 signature over OID
    uint64_t        timestamp;      // Signing timestamp
    uint32_t        trust_level;    // TRUST_SYSTEM, TRUST_USER, TRUST_UNTRUSTED
} object_signature_t;

typedef enum {
    TRUST_SYSTEM    = 0,    // Signed by Helios system key (kernel, core drivers)
    TRUST_USER      = 1,    // Signed by a user key (user-installed software)
    TRUST_UNTRUSTED = 2,    // No signature or unknown signer
} trust_level_t;
```

### 5.2 Execution Policy

The kernel enforces an execution policy based on trust level:

| Trust Level | Allowed Capabilities |
|------------|---------------------|
| `TRUST_SYSTEM` | Full: MMIO, DMA, IRQ, NPU, all syscalls |
| `TRUST_USER` | Standard: memory, IPC, storage, UI, inference |
| `TRUST_UNTRUSTED` | Sandboxed: memory only, no device access, limited IPC |

---

## 6. Memory Safety

### 6.1 Guard Pages

Every capability region is surrounded by unmapped guard pages:

```
[guard page - unmapped] [capability region] [guard page - unmapped]
     #PF on access           valid               #PF on access
```

This catches buffer overflows/underflows even without hardware bounds checking.

### 6.2 Stack Canaries

The kernel and all micro-programs use stack canaries:

```c
// Stack protector — randomized canary value per micro-program
uint64_t __stack_chk_guard;

void __stack_chk_fail(void) {
    // Stack smashing detected — terminate the micro-program immediately
    microprog_kill(current_microprog(), KILL_REASON_STACK_SMASH);
}
```

### 6.3 W^X Enforcement

No memory region is simultaneously writable and executable:

```c
// Enforce W^X: if a page is writable, it must not be executable, and vice versa
void vmm_enforce_wx(uint64_t virt_addr, uint64_t size, uint32_t perms) {
    if ((perms & CAP_PERM_WRITE) && (perms & CAP_PERM_EXEC)) {
        panic("W^X violation: region cannot be both writable and executable");
    }
}
```

---

## 7. Kernel Self-Protection

| Mechanism | Description |
|-----------|-------------|
| **KASLR** | Kernel Address Space Layout Randomization — randomize kernel base within the SASOS kernel region |
| **Stack Guard** | Per-core kernel stacks with guard pages |
| **Read-Only After Init** | Kernel data marked read-only after initialization (`.rodata`, page tables) |
| **NX for data** | All kernel data pages have NX bit set |
| **SMEP/SMAP** | Supervisor Mode Execution/Access Prevention — kernel cannot accidentally execute/read user-space memory |

```c
void kernel_enable_protections(void) {
    uint64_t cr4 = read_cr4();
    cr4 |= CR4_SMEP;   // Supervisor Mode Execution Prevention
    cr4 |= CR4_SMAP;   // Supervisor Mode Access Prevention
    write_cr4(cr4);
}
```

---

## 8. Cryptographic Primitives

All cryptographic implementations are constant-time to prevent side-channel attacks:

| Algorithm | Use | Implementation |
|-----------|-----|---------------|
| SHA-256 | Object content addressing, integrity verification | Software (hardware SHA-NI where available) |
| HMAC-SHA-256 | Capability token authentication | Software on top of SHA-256 |
| Ed25519 | Code signing, boot chain verification | Software (ref10 implementation) |
| RDRAND/RDSEED | Entropy source for key generation | Hardware instruction |
| AES-256-GCM | Future: encrypted objects at rest | Hardware AES-NI |

```c
// Constant-time memory comparison (prevents timing side channels)
int crypto_memcmp(const void *a, const void *b, size_t len) {
    const uint8_t *x = a, *y = b;
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= x[i] ^ y[i];
    }
    return diff;  // 0 if equal, non-zero if different
}
```

---

## 9. Threat Model

| Threat | Mitigation |
|--------|-----------|
| Malicious micro-program forges capability | HMAC with kernel-private key; cannot forge without key |
| Driver bug corrupts kernel memory | Driver runs in Ring 3; IOMMU prevents DMA to kernel pages |
| Buffer overflow in micro-program | Guard pages, stack canaries, W^X, capability bounds |
| Compromised bootloader | UEFI Secure Boot + kernel signature verification |
| Cold boot attack (DRAM remanence) | HMAC key changes every boot; sensitive data zeroed on shutdown |
| Side-channel (Spectre, Meltdown) | SASOS reduces attack surface (no cross-AS speculation); IBRS/IBPB for kernel entry |
| Physical access / DMA attack | IOMMU enforced for all DMA devices; no legacy DMA ports |

---

*Next: [10-NETWORKING.md](./10-NETWORKING.md) — TCP/IP Stack & Zero-Copy Networking*
