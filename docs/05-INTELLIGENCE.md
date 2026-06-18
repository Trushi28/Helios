# 05 — Bare-Metal NPU Scheduling & `sys_infer`

> **Subsystem:** Intelligence Layer  
> **Owner:** AI/Kernel team  
> **Dependencies:** PMM, capability system, PCIe driver model, scheduler  
> **Related:** [02-MEMORY.md](./02-MEMORY.md), [03-SCHEDULER.md](./03-SCHEDULER.md), [07-DRIVERS.md](./07-DRIVERS.md)

---

## 1. The Problem with Application-Layer AI

In every existing operating system, AI inference is an afterthought:

| Current Approach | Problems |
|-----------------|----------|
| Cloud API calls | Network latency (50–500 ms), privacy, internet dependency |
| User-space LLM (llama.cpp) | Fights OS for RAM, no priority isolation, user must install |
| GPU via CUDA/ROCm in user space | Driver overhead, memory fragmentation, no system-wide sharing |
| NPU via vendor SDK | Proprietary, application-specific, no OS integration |

Helios treats inference as a **first-class kernel resource** — like CPU time or physical memory.

---

## 2. Architecture Overview

```
┌──────────────────────────────────────────────────────────────────┐
│  User Micro-Programs                                             │
│                                                                  │
│  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌──────────────────┐   │
│  │  Shell   │  │  Editor │  │ Daemon  │  │ Code Analyzer    │   │
│  │         │  │         │  │         │  │                  │   │
│  │ sys_infer│  │ sys_infer│  │ sys_infer│  │ sys_infer       │   │
│  └────┬────┘  └────┬────┘  └────┬────┘  └────────┬─────────┘   │
│       │            │            │                 │              │
│       ▼            ▼            ▼                 ▼              │
│  ┌─────────────────────────────────────────────────────────┐     │
│  │              INFERENCE REQUEST QUEUE                     │     │
│  │         (priority-ordered, per-core submission)          │     │
│  └──────────────────────┬──────────────────────────────────┘     │
│                          │                                       │
│  ┌───────────────────────▼──────────────────────────────────┐    │
│  │              INFERENCE SCHEDULER (kernel)                 │    │
│  │                                                           │    │
│  │  ┌──────────────┐  ┌──────────────┐  ┌────────────────┐  │    │
│  │  │ Request      │  │ Context      │  │ Token Stream   │  │    │
│  │  │ Validator    │  │ Manager      │  │ Router         │  │    │
│  │  └──────────────┘  └──────────────┘  └────────────────┘  │    │
│  └───────────────────────┬──────────────────────────────────┘    │
│                          │                                       │
│  ┌───────────────────────▼──────────────────────────────────┐    │
│  │              NPU MEMORY ENCLAVE                           │    │
│  │                                                           │    │
│  │  ┌─────────────────────────────────────────────────┐      │    │
│  │  │  Model Weights (quantized, read-only)            │      │    │
│  │  │  4-bit GGUF / GGML format, ~2-4 GiB              │      │    │
│  │  └─────────────────────────────────────────────────┘      │    │
│  │  ┌─────────────────────────────────────────────────┐      │    │
│  │  │  KV Cache (per-request context windows)          │      │    │
│  │  │  Dynamically sized, ring-buffer managed           │      │    │
│  │  └─────────────────────────────────────────────────┘      │    │
│  │  ┌─────────────────────────────────────────────────┐      │    │
│  │  │  Scratch Buffers (intermediate activations)       │      │    │
│  │  └─────────────────────────────────────────────────┘      │    │
│  └──────────────────────────────────────────────────────────┘    │
│                          │                                       │
│  ┌───────────────────────▼──────────────────────────────────┐    │
│  │              HARDWARE BACKEND                             │    │
│  │  ┌──────┐  ┌──────────────┐  ┌────────────────────────┐  │    │
│  │  │ NPU  │  │ GPU Tensor   │  │ CPU (SIMD fallback)    │  │    │
│  │  │ (PCIe│  │ Cores (via   │  │ AVX-512/AVX2 matmul    │  │    │
│  │  │  or  │  │  compute     │  │ Slowest, always works  │  │    │
│  │  │ integ│  │  shaders)    │  │                        │  │    │
│  │  │ rated│  │              │  │                        │  │    │
│  │  └──────┘  └──────────────┘  └────────────────────────┘  │    │
│  └──────────────────────────────────────────────────────────┘    │
└──────────────────────────────────────────────────────────────────┘
```

---

## 3. NPU Memory Enclave

### 3.1 Physical Reservation

During boot, before the PMM is initialized, the kernel reserves a contiguous physical memory region for the NPU enclave. This region is **never** allocated to any micro-program or driver:

```c
#define NPU_ENCLAVE_MIN_SIZE  (1ULL * 1024 * 1024 * 1024)   // 1 GiB minimum
#define NPU_ENCLAVE_MAX_SIZE  (16ULL * 1024 * 1024 * 1024)  // 16 GiB maximum

typedef struct {
    phys_addr_t     phys_base;
    uint64_t        size;
    uint64_t        virt_base;      // Mapped in SASOS NPU enclave region

    struct {
        uint64_t    offset;         // Offset within enclave
        uint64_t    size;           // Size of weights blob
        bool        loaded;
    } weights;

    struct {
        uint64_t    offset;
        uint64_t    size;           // Total KV cache capacity
        uint64_t    used;
    } kv_cache;

    struct {
        uint64_t    offset;
        uint64_t    size;
    } scratch;

    spinlock_t      lock;
} npu_enclave_t;
```

### 3.2 Enclave Security

The NPU enclave is mapped in the SASOS with **kernel-only** page permissions. No user micro-program can access model weights or KV cache directly. All interaction is mediated through `sys_infer`:

- Page table: `PRESENT | WRITE | GLOBAL | NO_USER`
- No capability tokens are ever issued for the enclave region
- DMA to/from the enclave is only permitted through the NPU driver's IOMMU domain

---

## 4. Model Loading

### 4.1 Boot-Time Model Loading

The base model is loaded during the boot sequence from the ESP:

```c
void npu_load_model(boot_info_t *boot_info) {
    // 1. Read compressed model from ESP (stored during OS installation)
    //    File: /EFI/HELIOS/BASEMODEL.BIN
    //    Format: GGUF (quantized, 4-bit or 8-bit)

    // 2. Decompress into NPU enclave weight region
    //    Using LZ4 decompression (fast, low-overhead)

    // 3. Verify model integrity (SHA-256 of decompressed weights)

    // 4. Initialize tokenizer (BPE vocabulary table)

    // 5. Pre-warm: run a dummy inference to initialize KV cache structures

    // 6. Mark model as ready
    g_npu_enclave.weights.loaded = true;
}
```

### 4.2 Model Specification

The base model is optimized for **system-level intelligence** tasks:

| Property | Value |
|----------|-------|
| Architecture | Transformer (decoder-only) |
| Parameter count | 1B–3B (fits in 1–4 GiB quantized) |
| Quantization | 4-bit (Q4_K_M) or 8-bit (Q8_0) |
| Context window | 4096–8192 tokens |
| Vocabulary | 32K BPE tokens |
| Specialization | Code completion, command prediction, semantic search, summarization |
| Format | GGUF (standardized, self-describing) |

### 4.3 Hot-Swap Model Loading

The model can be replaced at runtime without rebooting:

```c
// Load a new model from an object in the graph store
int sys_npu_load_model(object_id_t model_oid);

// Query current model info
int sys_npu_model_info(npu_model_info_t *out);
```

---

## 5. The `sys_infer` System Call

### 5.1 Interface

```c
typedef struct {
    // Input
    cap_token_t     context_cap;    // Capability to input context buffer
    uint64_t        context_len;    // Length of context in bytes (UTF-8 text)
    uint32_t        max_tokens;     // Maximum tokens to generate
    float           temperature;    // Sampling temperature (0.0 = greedy)
    float           top_p;          // Nucleus sampling threshold
    uint32_t        flags;          // INFER_FLAG_STREAM, INFER_FLAG_JSON, etc.

    // Output
    cap_token_t     output_cap;     // Capability to output buffer
    uint64_t        output_max_len; // Maximum output buffer size

    // Callback (for streaming)
    uint64_t        callback_port;  // IPC port for token-by-token delivery
} sys_infer_request_t;

typedef struct {
    uint32_t        status;         // INFER_OK, INFER_TRUNCATED, INFER_ERROR
    uint64_t        tokens_generated;
    uint64_t        output_len;     // Actual bytes written to output buffer
    uint64_t        latency_ns;     // Total inference time
    uint64_t        prompt_tokens;  // Number of prompt tokens processed
} sys_infer_result_t;

// Synchronous inference (blocks until complete)
sys_infer_result_t sys_infer(sys_infer_request_t *request);

// Asynchronous inference (returns immediately, delivers via IPC port)
int sys_infer_async(sys_infer_request_t *request, uint64_t *request_id);

// Cancel an in-flight async request
int sys_infer_cancel(uint64_t request_id);
```

### 5.2 Request Flags

```c
#define INFER_FLAG_STREAM       (1 << 0)  // Stream tokens via IPC as generated
#define INFER_FLAG_JSON         (1 << 1)  // Constrain output to valid JSON
#define INFER_FLAG_GREEDY       (1 << 2)  // Greedy decoding (ignore temperature)
#define INFER_FLAG_NO_CACHE     (1 << 3)  // Don't use KV cache (fresh context)
#define INFER_FLAG_EMBED_ONLY   (1 << 4)  // Return embedding vector, not text
#define INFER_FLAG_HIGH_PRIO    (1 << 5)  // Elevated scheduling priority
```

### 5.3 Embedding API

For semantic search and similarity computation:

```c
typedef struct {
    cap_token_t     text_cap;       // Input text buffer
    uint64_t        text_len;
    cap_token_t     embedding_cap;  // Output: float32 embedding vector
    uint32_t        dimensions;     // Expected embedding dimensions
} sys_embed_request_t;

sys_infer_result_t sys_embed(sys_embed_request_t *request);
```

---

## 6. Inference Scheduler

### 6.1 Request Queue

Inference requests are priority-ordered:

```c
typedef enum {
    INFER_PRIO_CRITICAL   = 0,  // System-level inference (boot, recovery)
    INFER_PRIO_INTERACTIVE = 1, // User-facing (shell autocomplete, editor)
    INFER_PRIO_BACKGROUND  = 2, // Background tasks (code analysis, indexing)
    INFER_PRIO_BATCH       = 3, // Bulk processing
} infer_priority_t;

typedef struct infer_queue_entry {
    sys_infer_request_t     request;
    uint64_t                request_id;
    uint32_t                requester_mprog_id;
    infer_priority_t        priority;
    uint64_t                submit_tsc;
    struct list_head        link;
} infer_queue_entry_t;
```

### 6.2 Scheduling Policy

The inference scheduler runs on a **dedicated kernel thread** (pinned to a specific core if possible):

```
1. Dequeue highest-priority request
2. Validate requester's capability for context buffer
3. Tokenize input (BPE tokenization in kernel)
4. Check KV cache for reusable prefix
5. Submit to hardware backend:
   a. If NPU available → submit to NPU command queue
   b. Else if GPU compute available → submit compute shader dispatch
   c. Else → fallback to CPU SIMD (AVX-512/AVX2)
6. On completion:
   a. If streaming: route each token to requester's IPC port
   b. If synchronous: copy result to output buffer, wake requester
7. Update KV cache with new context
```

### 6.3 KV Cache Management

The KV cache stores pre-computed attention state, enabling fast continuation:

```c
typedef struct kv_cache_slot {
    uint32_t    owner_mprog_id;     // Who owns this context
    uint64_t    request_id;         // Which request populated this
    uint32_t    token_count;        // Number of tokens in this context
    uint64_t    last_access_tsc;    // For LRU eviction
    uint64_t    cache_offset;       // Offset within KV cache region
    uint64_t    cache_size;         // Size of this slot
    uint8_t     prefix_hash[16];    // Hash of the prompt prefix (for reuse)
    bool        active;
} kv_cache_slot_t;

#define KV_CACHE_MAX_SLOTS 64

// KV cache manager: LRU eviction when slots are full
kv_cache_slot_t *kv_cache_alloc(uint32_t token_count);
kv_cache_slot_t *kv_cache_find_prefix(const uint8_t *prefix_hash);
void             kv_cache_evict_lru(void);
```

---

## 7. Hardware Backend Abstraction

### 7.1 Backend Interface

```c
typedef struct npu_backend {
    const char *name;
    
    // Initialize the backend
    int  (*init)(void);
    
    // Submit a batch of matrix multiplications
    int  (*matmul)(const void *A, const void *B, void *C,
                   uint32_t M, uint32_t N, uint32_t K,
                   uint32_t dtype);
    
    // Run full transformer forward pass
    int  (*forward)(const void *weights, const void *input_tokens,
                    uint32_t seq_len, void *kv_cache, void *output_logits);
    
    // Synchronize (wait for all pending ops)
    void (*sync)(void);
    
    // Capabilities
    uint32_t max_batch_size;
    uint32_t max_seq_len;
    bool     supports_quantized;
    bool     supports_fp16;
} npu_backend_t;
```

### 7.2 CPU Backend (Fallback)

Always available. Uses AVX-512 or AVX2 SIMD for matrix operations:

```c
// CPU SIMD matmul — used when no NPU/GPU is available
void cpu_matmul_q4(const uint8_t *weights_q4, const float *input,
                   float *output, uint32_t M, uint32_t N, uint32_t K) {
    // Dequantize Q4 weights on-the-fly
    // AVX-512 VNNI or AVX2 VPMADDUBSW for int8 accumulation
    // Process 16 elements per SIMD lane
    ...
}
```

### 7.3 GPU Compute Backend

If a Vulkan 1.2+ GPU is present, we can use compute shaders for inference:

```c
// GPU compute backend — uses Vulkan compute shaders
// Requires GPU driver initialization (see 06-COMPOSITOR.md)
int gpu_backend_matmul(const void *A, const void *B, void *C,
                       uint32_t M, uint32_t N, uint32_t K,
                       uint32_t dtype) {
    // 1. Upload matrices to GPU VRAM (or use shared SASOS memory)
    // 2. Dispatch compute shader (matmul.comp)
    // 3. Wait for fence
    // 4. Read result back (or read directly from shared memory)
}
```

### 7.4 Dedicated NPU Backend

For systems with Intel NPU, AMD XDNA, or similar:

```c
// NPU backend — direct register-level programming
// Each NPU has a vendor-specific command interface
// We abstract behind the npu_backend_t interface
```

---

## 8. System-Wide Intelligence Services

Built on `sys_infer`, the kernel provides higher-level intelligence APIs:

### 8.1 Semantic Search

```c
// Index an object's content as an embedding in the graph store
int sys_semantic_index(object_id_t oid);

// Search all indexed objects by semantic similarity
int sys_semantic_search(const char *query, uint32_t max_results,
                        struct { object_id_t oid; float score; } *results);
```

### 8.2 Context-Aware Completions

```c
// Get completion suggestions for shell input
int sys_complete(const char *partial_input, uint32_t cursor_pos,
                 const char *context,   // e.g., current working collection
                 char completions[][256], uint32_t max_completions);
```

### 8.3 Object Summarization

```c
// Generate a natural-language summary of an object's contents
int sys_summarize(object_id_t oid, char *summary, uint32_t max_len);
```

---

## 9. Resource Limits & Fairness

To prevent a single micro-program from monopolizing the NPU:

```c
typedef struct {
    uint32_t    max_concurrent_requests;    // Max in-flight requests per µP
    uint32_t    max_tokens_per_request;     // Token generation limit
    uint64_t    max_compute_time_ns;        // Wall-clock limit per request
    uint64_t    daily_token_budget;         // Total tokens per day per µP
    uint64_t    tokens_used_today;
} infer_quota_t;

// Set quota for a micro-program (kernel or admin only)
int sys_infer_set_quota(uint32_t microprog_id, const infer_quota_t *quota);
```

---

## 10. Performance Targets

| Metric | Target | Notes |
|--------|--------|-------|
| Prompt processing (1K tokens, CPU) | < 500 ms | AVX-512, Q4_K_M quantized |
| Token generation (CPU) | > 20 tok/s | Single-core, 3B model |
| Prompt processing (GPU) | < 50 ms | Compute shader backend |
| Token generation (GPU) | > 100 tok/s | 3B model, fp16 |
| sys_infer syscall overhead | < 5 µs | Kernel entry + queue insertion |
| Embedding (512-dim, 128 tokens) | < 10 ms | GPU backend |

---

*Next: [06-COMPOSITOR.md](./06-COMPOSITOR.md) — GPU Vertex-Matrix Compositor*
