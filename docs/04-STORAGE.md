# 04 — NVMe Driver & Cryptographic Object Graph File System

> **Subsystem:** Storage  
> **Owner:** Storage team  
> **Dependencies:** PCIe ECAM, IOMMU, PMM, capability system  
> **Related:** [02-MEMORY.md](./02-MEMORY.md), [07-DRIVERS.md](./07-DRIVERS.md), [09-SECURITY.md](./09-SECURITY.md)

---

## 1. Design Philosophy

Helios has **no traditional file system**. There are no files, no directories, no paths, no inodes, no superblocks. Instead:

- **Layer 1: NVMe Block Driver** — Talks raw NVMe protocol to the SSD controller
- **Layer 2: Object Store Engine** — Content-addressed immutable object storage on raw blocks
- **Layer 3: Semantic Graph** — Directed acyclic graph (DAG) of metadata relationships
- **Layer 4: Query Interface** — Graph-traversal API exposed via syscalls

---

## 2. NVMe Driver Architecture

### 2.1 NVMe Queue Model

NVMe uses a **submission/completion queue** model with no legacy command set:

```
CPU                              NVMe Controller
 │                                      │
 │  1. Write command to SQ tail ──────▶ │
 │  2. Ring SQ doorbell ─────────────▶  │  (controller fetches command via DMA)
 │                                      │  (controller processes I/O)
 │  ◀──── 3. Write completion to CQ ── │  (controller writes CQ entry via DMA)
 │  ◀──── 4. MSI-X interrupt ───────── │
 │  5. Read CQ entry                    │
 │  6. Ring CQ doorbell ─────────────▶  │
```

### 2.2 Queue Layout

```c
#define NVME_ADMIN_QUEUE_SIZE   64      // Admin queue depth
#define NVME_IO_QUEUE_SIZE      1024    // I/O queue depth (per-core)
#define NVME_MAX_IO_QUEUES      256     // One per core

typedef struct nvme_sq_entry {
    uint32_t cdw0;          // Command Dword 0: opcode, fused, psdt, cid
    uint32_t nsid;          // Namespace ID
    uint64_t _rsvd;
    uint64_t mptr;          // Metadata pointer
    uint64_t prp1;          // PRP Entry 1 (physical address of data)
    uint64_t prp2;          // PRP Entry 2 (or PRP list pointer)
    uint32_t cdw10;         // Command-specific
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} __attribute__((packed)) nvme_sq_entry_t;

_Static_assert(sizeof(nvme_sq_entry_t) == 64, "NVMe SQ entry must be 64 bytes");

typedef struct nvme_cq_entry {
    uint32_t result;        // Command-specific result
    uint32_t _rsvd;
    uint16_t sq_head;       // SQ head pointer
    uint16_t sq_id;         // SQ identifier
    uint16_t cmd_id;        // Command identifier
    uint16_t status;        // Status field (phase bit, status code)
} __attribute__((packed)) nvme_cq_entry_t;

_Static_assert(sizeof(nvme_cq_entry_t) == 16, "NVMe CQ entry must be 16 bytes");
```

### 2.3 Controller Initialization Sequence

```c
void nvme_init(pcie_device_t *dev) {
    // 1. Map BAR0 (controller registers) as uncacheable MMIO
    volatile nvme_regs_t *regs = mmio_map_bar(dev, 0, DRIVER_MPROG_ID);

    // 2. Disable controller (CC.EN = 0), wait for CSTS.RDY = 0
    regs->cc &= ~NVME_CC_EN;
    while (regs->csts & NVME_CSTS_RDY) cpu_pause();

    // 3. Configure Admin Queue
    //    - Allocate physically contiguous SQ and CQ buffers
    //    - Write base addresses to ASQ and ACQ registers
    //    - Set AQA (Admin Queue Attributes) with queue sizes
    phys_addr_t admin_sq = pmm_alloc_pages(0, ALLOC_CONTIGUOUS);
    phys_addr_t admin_cq = pmm_alloc_pages(0, ALLOC_CONTIGUOUS);
    regs->asq = admin_sq;
    regs->acq = admin_cq;
    regs->aqa = ((NVME_ADMIN_QUEUE_SIZE - 1) << 16) |
                (NVME_ADMIN_QUEUE_SIZE - 1);

    // 4. Configure CC: I/O CQ entry size, I/O SQ entry size, page size
    regs->cc = NVME_CC_IOCQES(4) |     // 2^4 = 16 bytes
               NVME_CC_IOSQES(6) |     // 2^6 = 64 bytes
               NVME_CC_MPS(0)   |      // 4 KiB pages
               NVME_CC_CSS_NVM;        // NVM command set

    // 5. Enable controller (CC.EN = 1), wait for CSTS.RDY = 1
    regs->cc |= NVME_CC_EN;
    while (!(regs->csts & NVME_CSTS_RDY)) cpu_pause();

    // 6. Identify Controller (admin command opcode 0x06)
    nvme_identify_controller(regs);

    // 7. Identify Namespace (admin command opcode 0x06, CNS=0)
    nvme_identify_namespace(regs, 1);

    // 8. Create I/O Completion Queues (one per core)
    for (uint32_t i = 0; i < g_cpu_count; i++) {
        nvme_create_io_cq(regs, i + 1, NVME_IO_QUEUE_SIZE, i);
    }

    // 9. Create I/O Submission Queues (one per core, mapped to CQ)
    for (uint32_t i = 0; i < g_cpu_count; i++) {
        nvme_create_io_sq(regs, i + 1, NVME_IO_QUEUE_SIZE, i + 1);
    }

    // 10. Set up MSI-X interrupt vectors (one per I/O queue + one for admin)
    nvme_setup_msix(dev, g_cpu_count + 1);

    // 11. Set up IOMMU DMA domain for the controller
    iommu_setup_device(dev, &nvme_dma_cap);
}
```

### 2.4 Per-Core I/O Queues

Each CPU core has its own I/O submission/completion queue pair. This eliminates all lock contention for I/O submission:

```c
typedef struct nvme_io_queue {
    // Submission Queue
    nvme_sq_entry_t    *sq_base;        // Virtual address (capability-protected)
    phys_addr_t         sq_phys;        // Physical address (for DMA)
    uint16_t            sq_tail;        // Producer index
    uint16_t            sq_size;

    // Completion Queue
    nvme_cq_entry_t    *cq_base;
    phys_addr_t         cq_phys;
    uint16_t            cq_head;        // Consumer index
    uint16_t            cq_size;
    uint8_t             cq_phase;       // Phase tag for completion detection

    // Doorbell register offsets
    volatile uint32_t  *sq_doorbell;
    volatile uint32_t  *cq_doorbell;

    // Pending command tracking
    struct {
        void           *callback_ctx;
        void          (*callback)(void *ctx, nvme_cq_entry_t *cqe);
        bool            in_use;
    } pending[NVME_IO_QUEUE_SIZE];

    uint16_t            next_cmd_id;
    uint32_t            core_id;
} nvme_io_queue_t;
```

### 2.5 Async I/O API

```c
typedef void (*nvme_callback_t)(void *ctx, nvme_cq_entry_t *completion);

// Submit an async read
int nvme_read_async(uint64_t lba, uint32_t block_count,
                    phys_addr_t buffer_phys,
                    nvme_callback_t callback, void *ctx);

// Submit an async write
int nvme_write_async(uint64_t lba, uint32_t block_count,
                     phys_addr_t buffer_phys,
                     nvme_callback_t callback, void *ctx);

// Synchronous wrappers (block until completion)
int nvme_read_sync(uint64_t lba, uint32_t block_count, void *buffer);
int nvme_write_sync(uint64_t lba, uint32_t block_count, const void *buffer);

// Poll for completions (called from interrupt handler or poll loop)
void nvme_poll_completions(nvme_io_queue_t *queue);
```

---

## 3. Object Store Engine

### 3.1 Concepts

| Concept | Description |
|---------|-------------|
| **Object** | An immutable blob of bytes, identified by SHA-256 hash of its contents |
| **Object ID (OID)** | 32-byte SHA-256 hash. Globally unique, content-derived |
| **Vertex** | A metadata node in the graph — may reference an object or be pure metadata |
| **Edge** | A typed, directed link between two vertices |
| **Transaction** | An atomic batch of object writes + graph mutations |
| **Snapshot** | A root vertex capturing the entire graph state at a point in time |

### 3.2 Object ID Computation

```c
#include "sha256.h"

typedef struct {
    uint8_t bytes[32];
} object_id_t;

object_id_t compute_oid(const void *data, size_t length) {
    object_id_t oid;
    sha256_context ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, length);
    sha256_final(&ctx, oid.bytes);
    return oid;
}
```

Content addressing guarantees:
- **Deduplication** — identical objects are stored exactly once
- **Integrity** — any bit flip changes the hash, detecting corruption
- **Immutability** — objects are never modified, only new objects are created

### 3.3 On-Disk Layout

The object store occupies the second GPT partition as a raw block device:

```
NVMe Namespace LBA Layout
══════════════════════════════════════════════════

LBA 0 — LBA 7:        Superblock (4 KiB)
LBA 8 — LBA N:        Object Extent Bitmap (tracks allocated extents)
LBA N+1 — LBA M:      Vertex Table (fixed-size vertex records)
LBA M+1 — LBA P:      Edge Table (fixed-size edge records)
LBA P+1 — LBA END:    Object Data Extents (variable-size object blobs)
```

### 3.4 Superblock

```c
#define OBJSTORE_MAGIC 0x48454C4F424A5354ULL  // "HELOBJST"

typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t block_size;            // Typically 4096
    uint64_t total_blocks;          // Total blocks in partition
    uint64_t bitmap_start_lba;
    uint64_t bitmap_block_count;
    uint64_t vtable_start_lba;
    uint64_t vtable_block_count;
    uint64_t etable_start_lba;
    uint64_t etable_block_count;
    uint64_t data_start_lba;
    uint64_t data_block_count;
    uint64_t object_count;          // Total stored objects
    uint64_t vertex_count;          // Total vertices
    uint64_t edge_count;            // Total edges
    object_id_t root_snapshot;      // OID of current root snapshot vertex
    uint64_t creation_timestamp;
    uint64_t last_txn_id;           // Monotonic transaction counter
    uint8_t  _reserved[3896];       // Pad to 4 KiB
} __attribute__((packed)) objstore_superblock_t;

_Static_assert(sizeof(objstore_superblock_t) == 4096, "Superblock must be 4 KiB");
```

### 3.5 Vertex Record

```c
typedef enum {
    VERTEX_TYPE_OBJECT      = 0,    // References a data object (has OID)
    VERTEX_TYPE_TAG         = 1,    // Pure metadata tag (e.g., "project:helios")
    VERTEX_TYPE_COLLECTION  = 2,    // Group vertex (similar concept to a "folder")
    VERTEX_TYPE_SNAPSHOT    = 3,    // System-wide snapshot root
    VERTEX_TYPE_SCHEMA      = 4,    // Schema definition vertex
    VERTEX_TYPE_EXECUTABLE  = 5,    // Executable micro-program object
    VERTEX_TYPE_USER        = 6,    // User identity vertex
} vertex_type_t;

typedef struct {
    uint64_t        vertex_id;          // Auto-incremented unique ID
    vertex_type_t   type;               // Vertex type
    uint32_t        flags;              // VERTEX_FLAG_DELETED, etc.
    object_id_t     oid;                // SHA-256 of referenced object (if type == OBJECT)
    uint64_t        data_lba;           // Starting LBA of object data
    uint64_t        data_size;          // Object size in bytes
    uint64_t        created_txn;        // Transaction ID when created
    uint64_t        created_timestamp;  // Nanosecond timestamp
    char            label[64];          // Human-readable label
    uint64_t        edge_head_offset;   // Offset into edge table for outgoing edges
    uint32_t        out_edge_count;     // Number of outgoing edges
    uint32_t        in_edge_count;      // Number of incoming edges
    uint8_t         _reserved[52];      // Pad to 256 bytes
} __attribute__((packed)) vertex_record_t;

_Static_assert(sizeof(vertex_record_t) == 256, "Vertex record must be 256 bytes");
```

### 3.6 Edge Record

```c
typedef enum {
    EDGE_TYPE_CONTAINS      = 0,    // Collection → Object (like "folder contains file")
    EDGE_TYPE_AUTHORED_BY   = 1,    // Object → User
    EDGE_TYPE_DEPENDS_ON    = 2,    // Object → Object (code dependency)
    EDGE_TYPE_COMPILED_FROM = 3,    // Executable → Source object
    EDGE_TYPE_TAGGED        = 4,    // Object → Tag
    EDGE_TYPE_SNAPSHOT_OF   = 5,    // Snapshot → previous snapshot
    EDGE_TYPE_CHILD_OF      = 6,    // Hierarchical relationship
    EDGE_TYPE_SCHEMA_IS     = 7,    // Object → Schema
    EDGE_TYPE_CUSTOM        = 255,  // User-defined edge type
} edge_type_t;

typedef struct {
    uint64_t    edge_id;            // Unique edge ID
    uint64_t    src_vertex_id;      // Source vertex
    uint64_t    dst_vertex_id;      // Destination vertex
    edge_type_t type;               // Relationship type
    uint32_t    flags;
    uint64_t    created_txn;        // Transaction ID
    char        label[24];          // Optional label (e.g., "v2.1.0")
    uint8_t     _reserved[12];      // Pad to 64 bytes
} __attribute__((packed)) edge_record_t;

_Static_assert(sizeof(edge_record_t) == 64, "Edge record must be 64 bytes");
```

---

## 4. Transactions & Append-Only Semantics

### 4.1 Transaction Model

All mutations to the object store are batched in **atomic transactions**:

```c
typedef struct {
    uint64_t    txn_id;             // Monotonic transaction ID
    uint64_t    timestamp;          // Nanosecond timestamp
    uint32_t    op_count;           // Number of operations
    struct {
        enum { TXN_OP_ADD_OBJECT, TXN_OP_ADD_VERTEX, TXN_OP_ADD_EDGE,
               TXN_OP_REMOVE_EDGE } type;
        union {
            struct { object_id_t oid; uint64_t data_lba; uint64_t size; } add_object;
            struct { vertex_record_t vertex; } add_vertex;
            struct { edge_record_t edge; } add_edge;
            struct { uint64_t edge_id; } remove_edge;
        };
    } ops[64];  // Max 64 ops per transaction
} transaction_t;
```

### 4.2 Append-Only Guarantees

- **Objects are never overwritten.** Modifying a document creates a new object with a new OID.
- **Vertices are never deleted** — they are marked with `VERTEX_FLAG_DELETED` (soft delete).
- **Edges can be removed**, but removal is logged in the transaction journal.
- **The superblock is double-buffered** — alternating between LBA 0 and LBA 4, so a crash during superblock write never corrupts both copies.

### 4.3 Crash Recovery

On boot, the object store engine:
1. Reads both superblock copies, selects the one with the higher `last_txn_id`
2. Verifies the root snapshot OID is reachable
3. Replays any incomplete transaction from the journal (WAL)
4. Marks the store as clean

---

## 5. Snapshot & Time-Travel

### 5.1 Automatic Snapshots

The system creates snapshot vertices at configurable intervals:

```c
// Create a snapshot of the entire graph state
object_id_t objstore_snapshot(const char *label);

// List all snapshots
void objstore_list_snapshots(snapshot_entry_t *out, uint32_t *count);

// Restore the graph to a previous snapshot (creates a new snapshot that
// points to the old state — the current state is not lost)
object_id_t objstore_restore(object_id_t snapshot_oid);

// Diff two snapshots — returns lists of added/removed/modified vertices
void objstore_diff(object_id_t snap_a, object_id_t snap_b,
                   graph_diff_t *out);
```

### 5.2 Garbage Collection

Since objects are never deleted, storage will eventually fill. A background GC micro-program:

1. Walks the graph from the current root snapshot
2. Marks all reachable objects
3. Objects not reachable from any retained snapshot are eligible for reclamation
4. Reclaimed extents are added back to the free bitmap

Snapshot retention policy is user-configurable:
- Keep all snapshots from the last 24 hours
- Keep daily snapshots for the last 30 days
- Keep monthly snapshots indefinitely

---

## 6. Graph Query Interface

### 6.1 Syscalls

```c
// Store a new object, returns its content-derived OID
object_id_t sys_obj_store(const void *data, uint64_t size);

// Retrieve an object by OID
int sys_obj_load(object_id_t oid, void *buffer, uint64_t buffer_size);

// Check if an object exists
bool sys_obj_exists(object_id_t oid);

// Create a vertex
uint64_t sys_vertex_create(vertex_type_t type, const char *label,
                           object_id_t *oid /* NULL for metadata-only */);

// Create an edge between vertices
uint64_t sys_edge_create(uint64_t src, uint64_t dst,
                         edge_type_t type, const char *label);

// Query: find vertices by label pattern
int sys_query_vertices(const char *label_pattern, vertex_type_t type_filter,
                       uint64_t *results, uint32_t max_results);

// Query: traverse edges from a vertex
int sys_query_edges(uint64_t vertex_id, edge_type_t type_filter,
                    enum { EDGE_DIR_OUT, EDGE_DIR_IN, EDGE_DIR_BOTH } direction,
                    uint64_t *results, uint32_t max_results);

// Query: find path between two vertices (BFS)
int sys_query_path(uint64_t src_vertex, uint64_t dst_vertex,
                   uint64_t *path, uint32_t max_path_len);

// Semantic query: find vertices related to a concept (via sys_infer)
int sys_query_semantic(const char *natural_language_query,
                       uint64_t *results, uint32_t max_results);
```

### 6.2 Example: Traditional vs. Helios

**Traditional OS — saving a source file:**
```
write("/home/user/projects/helios/src/kernel/main.c", data, len)
  → VFS lookup: traverse 5 directory inodes
  → Allocate data blocks
  → Update inode timestamps
  → Update directory entry
  → fsync() for durability
```

**Helios — storing a source object:**
```
oid = sys_obj_store(data, len)                               // Store immutable object
vid = sys_vertex_create(VERTEX_TYPE_OBJECT, "main.c", &oid)  // Create vertex
sys_edge_create(vid, project_vid, EDGE_TYPE_CHILD_OF, NULL)  // Link to project
sys_edge_create(vid, author_vid, EDGE_TYPE_AUTHORED_BY, NULL)// Link to author
sys_edge_create(vid, c_lang_vid, EDGE_TYPE_TAGGED, "lang")   // Tag as C source
```

The object is now discoverable by project, by author, by language, by content hash — without any directory hierarchy.

---

## 7. In-Memory Cache

The object store maintains a hot cache of frequently-accessed vertex/edge records and recently-used object data in the SASOS:

```c
typedef struct objcache {
    // Vertex LRU cache
    struct {
        vertex_record_t  *entries;
        uint32_t          capacity;     // Number of cached vertices
        uint32_t          count;
        struct hash_table index;        // vertex_id → cache slot
    } vertices;

    // Edge LRU cache
    struct {
        edge_record_t    *entries;
        uint32_t          capacity;
        uint32_t          count;
        struct hash_table index;        // edge_id → cache slot
    } edges;

    // Object data page cache
    struct {
        struct hash_table oid_to_pages; // OID → list of cached pages
        uint64_t          total_pages;
        uint64_t          max_pages;    // Eviction threshold
    } data_cache;

    spinlock_t lock;
} objcache_t;
```

The cache is mapped in the `Object Graph Cache` region of the SASOS layout (see [02-MEMORY.md](./02-MEMORY.md)).

---

## 8. Integrity & Verification

### 8.1 Read-Time Verification

Every time an object is read from disk, its SHA-256 hash is recomputed and compared against the stored OID. A mismatch indicates data corruption:

```c
int obj_load_verified(object_id_t expected_oid, void *buffer, uint64_t size) {
    int ret = nvme_read_sync(oid_to_lba(expected_oid), size / BLOCK_SIZE, buffer);
    if (ret < 0) return ret;

    object_id_t actual_oid = compute_oid(buffer, size);
    if (memcmp(&actual_oid, &expected_oid, 32) != 0) {
        return -ERR_INTEGRITY_VIOLATION;
    }
    return 0;
}
```

### 8.2 Background Scrubbing

A low-priority GC micro-program periodically reads and verifies random objects, detecting silent data corruption (bit rot) before it becomes a problem.

---

*Next: [05-INTELLIGENCE.md](./05-INTELLIGENCE.md) — Bare-Metal NPU Scheduling & sys_infer*
