# 03 — SMP-Aware Micro-Program Scheduler

> **Subsystem:** Scheduler  
> **Owner:** Kernel team  
> **Dependencies:** x2APIC, MADT, TSC, per-core data structures  
> **Related:** [02-MEMORY.md](./02-MEMORY.md), [14-INTERRUPTS.md](./14-INTERRUPTS.md), [08-IPC.md](./08-IPC.md)

---

## 1. Design Goals

| Goal | Rationale |
|------|-----------|
| **Per-core run queues** | Eliminate global lock contention; each core schedules independently |
| **Work stealing** | Idle cores steal tasks from busy cores for load balance |
| **Cache affinity** | Prefer re-scheduling a micro-program on the same core |
| **Priority classes** | Real-time (compositor, audio) > interactive (shell) > batch (background) |
| **O(1) dispatch** | Constant-time next-task selection using bitmap + per-priority queues |
| **Tickless operation** | No periodic timer interrupt; use one-shot x2APIC timer |

---

## 2. SMP Bring-Up (AP Core Initialization)

### 2.1 INIT-SIPI-SIPI Protocol

After the BSP (Bootstrap Processor) completes kernel early init, it awakens Application Processors using the standard x2APIC IPI sequence:

```
BSP                                     AP Core
 │                                        │
 │  ──── INIT IPI ──────────────────────▶ │  (resets AP to real mode)
 │       (wait 10 ms)                     │
 │  ──── SIPI (vector = 0xNN) ─────────▶ │  (AP begins executing at 0xNN000)
 │       (wait 200 µs)                    │
 │  ──── SIPI (vector = 0xNN) ─────────▶ │  (redundant, per Intel spec)
 │       (wait 200 µs)                    │
 │                                        │  AP trampoline:
 │                                        │  1. Switch to protected mode
 │                                        │  2. Switch to long mode
 │                                        │  3. Load GDT, IDT (same as BSP)
 │                                        │  4. Load CR3 (same SASOS PML4)
 │                                        │  5. Initialize local x2APIC
 │                                        │  6. Signal BSP via atomic flag
 │  ◀──── AP ready signal ──────────────  │
 │                                        │  7. Enter scheduler_idle_loop()
```

### 2.2 AP Trampoline Code

The AP trampoline must be located below 1 MiB (real-mode addressable). We allocate a single 4 KiB page at a known address (e.g., 0x8000):

```c
// AP trampoline layout (must fit in one 4 KiB page)
// Located at physical address 0x8000
typedef struct {
    uint8_t  code_16[256];      // 16-bit real mode → protected mode stub
    uint8_t  code_32[256];      // 32-bit protected → long mode stub  
    uint8_t  code_64[512];      // 64-bit init: load GDT/IDT/CR3, call ap_entry()
    uint64_t pml4_phys;         // CR3 value (same as BSP)
    uint64_t gdt_ptr;           // GDT pointer (same as BSP)
    uint64_t idt_ptr;           // IDT pointer (same as BSP)
    uint64_t stack_top;         // Per-AP stack (allocated by BSP)
    uint64_t ap_entry_fn;       // Virtual address of ap_entry() in kernel
    volatile uint32_t ap_ready; // Atomic flag: AP sets to 1 when ready
} __attribute__((packed)) ap_trampoline_t;
```

### 2.3 x2APIC Configuration

Helios requires x2APIC mode (MSR-based interface). We do **not** fall back to xAPIC (MMIO-based).

```c
void x2apic_init(void) {
    // Verify x2APIC support
    uint32_t ecx;
    __cpuid(1, NULL, NULL, &ecx, NULL);
    assert(ecx & CPUID_X2APIC_BIT);  // bit 21

    // Enable x2APIC mode
    uint64_t apic_base = rdmsr(MSR_IA32_APIC_BASE);
    apic_base |= APIC_BASE_X2APIC_ENABLE | APIC_BASE_GLOBAL_ENABLE;
    wrmsr(MSR_IA32_APIC_BASE, apic_base);

    // Set spurious interrupt vector, enable APIC
    wrmsr(MSR_X2APIC_SIVR, APIC_SIVR_ENABLE | SPURIOUS_VECTOR);

    // Configure LINT0, LINT1
    wrmsr(MSR_X2APIC_LVT_LINT0, APIC_LVT_MASKED);
    wrmsr(MSR_X2APIC_LVT_LINT1, APIC_DELIVERY_NMI);

    // Read our x2APIC ID
    uint32_t apic_id = (uint32_t)rdmsr(MSR_X2APIC_ID);
}

// Send IPI (x2APIC uses MSR, no ICR high/low split)
void x2apic_send_ipi(uint32_t target_apic_id, uint8_t vector, uint8_t delivery_mode) {
    uint64_t icr = ((uint64_t)target_apic_id << 32) |
                   (delivery_mode << 8) |
                   vector;
    wrmsr(MSR_X2APIC_ICR, icr);
}
```

---

## 3. Micro-Program Control Block

Every schedulable entity in Helios is a **micro-program** (µP). There are no processes or threads in the traditional sense — a micro-program is a capability-bounded execution context.

```c
typedef enum {
    MPROG_STATE_RUNNING,        // Currently executing on a core
    MPROG_STATE_READY,          // In a run queue, waiting to be dispatched
    MPROG_STATE_BLOCKED,        // Waiting on I/O, IPC, or sys_infer
    MPROG_STATE_SLEEPING,       // Voluntarily sleeping (timed)
    MPROG_STATE_DEAD,           // Terminated, awaiting cleanup
} mprog_state_t;

typedef enum {
    PRIORITY_REALTIME   = 0,    // Compositor, audio, interrupt handlers
    PRIORITY_INTERACTIVE = 1,   // Shell, UI micro-programs
    PRIORITY_NORMAL     = 2,    // Default priority
    PRIORITY_BATCH      = 3,    // Background tasks, compilers
    PRIORITY_IDLE       = 4,    // Only runs when nothing else is ready
    NUM_PRIORITY_LEVELS = 5,
} priority_t;

typedef struct microprogram {
    // --- Identity ---
    uint32_t            id;             // Unique micro-program ID
    char                name[64];       // Human-readable name
    mprog_state_t       state;
    priority_t          priority;

    // --- CPU Context ---
    struct {
        uint64_t rax, rbx, rcx, rdx;
        uint64_t rsi, rdi, rbp, rsp;
        uint64_t r8, r9, r10, r11;
        uint64_t r12, r13, r14, r15;
        uint64_t rip, rflags;
        uint64_t fs_base, gs_base;      // Per-µP thread-local storage
        // FPU/SSE/AVX state saved via XSAVE
        uint8_t  xsave_area[4096] __attribute__((aligned(64)));
    } ctx;

    // --- Capabilities ---
    cap_set_t          *cap_set;        // Set of active capability tokens
    uint32_t            cap_count;

    // --- Scheduling ---
    uint32_t            affinity_mask;  // Preferred core bitmask
    uint32_t            last_core;      // Last core this µP ran on
    uint64_t            time_slice_ns;  // Remaining time slice in nanoseconds
    uint64_t            total_runtime_ns;
    uint64_t            wake_time_tsc;  // TSC value to wake (if sleeping)

    // --- IPC ---
    struct ipc_port    *port;           // IPC message receive port
    struct wait_queue  *wait_queue;     // What this µP is blocked on

    // --- Hierarchy ---
    uint32_t            parent_id;      // Parent micro-program ID
    struct list_head    children;       // Child micro-programs
    struct list_head    sibling;        // Sibling link

    // --- Scheduler Linkage ---
    struct list_head    run_queue_link; // Link in the per-core run queue
    struct list_head    global_list;    // Link in global micro-program list

    // --- Statistics ---
    uint64_t            syscall_count;
    uint64_t            page_fault_count;
    uint64_t            ipc_send_count;
    uint64_t            ipc_recv_count;
    uint64_t            infer_request_count;
} microprogram_t;
```

---

## 4. Per-Core Scheduler

### 4.1 Data Structure

Each CPU core maintains a private scheduler structure:

```c
typedef struct per_core_scheduler {
    uint32_t            core_id;
    uint32_t            x2apic_id;

    // Current running micro-program (NULL if idle)
    microprogram_t     *current;

    // Per-priority doubly-linked ready queues
    struct list_head    ready_queues[NUM_PRIORITY_LEVELS];
    uint32_t            ready_counts[NUM_PRIORITY_LEVELS];

    // Bitmap: bit N set if ready_queues[N] is non-empty
    uint32_t            priority_bitmap;

    // Idle thread for this core
    microprogram_t     *idle_thread;

    // One-shot timer configuration
    uint64_t            timer_deadline_tsc;

    // Work-stealing state
    uint64_t            steal_attempt_count;
    uint64_t            steal_success_count;

    // Statistics
    uint64_t            context_switch_count;
    uint64_t            idle_time_ns;

    // Lock for cross-core manipulation (work stealing)
    spinlock_t          lock;
} per_core_scheduler_t;

// Per-core data is accessed via GS segment base (set during SMP init)
static inline per_core_scheduler_t *this_core_sched(void) {
    per_core_scheduler_t *s;
    asm volatile("mov %%gs:0, %0" : "=r"(s));
    return s;
}
```

### 4.2 O(1) Dispatch Algorithm

```c
microprogram_t *scheduler_pick_next(per_core_scheduler_t *sched) {
    if (sched->priority_bitmap == 0) {
        return sched->idle_thread;  // Nothing to run
    }

    // Find highest priority (lowest bit index) with ready tasks
    uint32_t highest = __builtin_ctz(sched->priority_bitmap);

    // Dequeue from front of that priority's ready queue
    microprogram_t *next = list_first_entry(
        &sched->ready_queues[highest],
        microprogram_t, run_queue_link
    );
    list_del(&next->run_queue_link);
    sched->ready_counts[highest]--;

    if (list_empty(&sched->ready_queues[highest])) {
        sched->priority_bitmap &= ~(1u << highest);
    }

    return next;
}
```

### 4.3 Time Slice Policy

| Priority | Base Time Slice | Preemption |
|----------|----------------|------------|
| REALTIME | 1 ms | Preempts everything below |
| INTERACTIVE | 4 ms | Preempts normal and below |
| NORMAL | 10 ms | Standard round-robin within priority |
| BATCH | 20 ms | Long slices, low preemption priority |
| IDLE | ∞ | Only runs when all queues are empty |

Time slices are enforced via the **x2APIC one-shot timer**:

```c
void scheduler_arm_timer(uint64_t ns) {
    // Convert nanoseconds to TSC ticks using calibrated TSC frequency
    uint64_t ticks = (ns * g_tsc_freq_mhz) / 1000;
    
    // Program x2APIC timer in one-shot mode
    wrmsr(MSR_X2APIC_LVT_TIMER, TIMER_VECTOR);  // No periodic bit
    wrmsr(MSR_X2APIC_TIMER_DIV, APIC_TIMER_DIV_1);
    wrmsr(MSR_X2APIC_TIMER_INIT, ticks);
}
```

---

## 5. Work Stealing

When a core's run queues are all empty, it attempts to steal work from other cores:

```c
bool scheduler_try_steal(per_core_scheduler_t *thief) {
    // Try each core, starting from a random offset to avoid thundering herd
    uint32_t start = rdtsc() % g_cpu_count;

    for (uint32_t i = 0; i < g_cpu_count; i++) {
        uint32_t victim_id = (start + i) % g_cpu_count;
        if (victim_id == thief->core_id) continue;

        per_core_scheduler_t *victim = &g_schedulers[victim_id];

        if (!spinlock_try_lock(&victim->lock)) continue;

        // Steal from lowest-priority non-empty queue of victim
        for (int p = NUM_PRIORITY_LEVELS - 1; p >= 0; p--) {
            if (victim->ready_counts[p] > 1) {
                // Steal from back of queue (oldest task, coldest cache)
                microprogram_t *stolen = list_last_entry(
                    &victim->ready_queues[p],
                    microprogram_t, run_queue_link
                );
                list_del(&stolen->run_queue_link);
                victim->ready_counts[p]--;
                if (victim->ready_counts[p] == 0)
                    victim->priority_bitmap &= ~(1u << p);

                spinlock_unlock(&victim->lock);

                // Enqueue on thief's queue
                scheduler_enqueue(thief, stolen);
                thief->steal_success_count++;
                return true;
            }
        }

        spinlock_unlock(&victim->lock);
    }

    thief->steal_attempt_count++;
    return false;
}
```

### 5.1 NUMA-Aware Stealing

On NUMA systems, the steal order prefers cores on the same NUMA node:

```
Steal Order: same-core siblings → same-NUMA-node cores → remote-NUMA-node cores
```

---

## 6. Context Switch Path

The full context switch is extremely lightweight because there is no page table swap:

```
Timer IRQ fires (or yield syscall)
  │
  ├─ Save current µP registers (GPRs + XSAVE)
  │   Cost: ~50 cycles (register save)
  │
  ├─ scheduler_pick_next() → O(1) bitmap scan
  │   Cost: ~20 cycles
  │
  ├─ cap_activate(next->cap_set)
  │   Cost: ~30 cycles (load bounds into per-core cap table)
  │
  ├─ Restore next µP registers (GPRs + XRSTOR)
  │   Cost: ~50 cycles (register restore)
  │
  ├─ Update GS base for per-µP TLS
  │   Cost: ~10 cycles (WRFSBASE)
  │
  └─ IRETQ
      Cost: ~20 cycles

  TOTAL: ~180 cycles (~60 ns at 3 GHz)
  vs. Traditional OS: ~1000-5000 cycles (TLB flush + page table swap)
```

---

## 7. Scheduler Syscalls

```c
// Yield the current time slice voluntarily
void sys_yield(void);

// Sleep for a specified duration (nanoseconds)
void sys_sleep(uint64_t nanoseconds);

// Set scheduling priority (requires CAP_PERM_SCHED or self-only)
int sys_set_priority(uint32_t microprog_id, priority_t priority);

// Set CPU affinity mask
int sys_set_affinity(uint32_t microprog_id, uint32_t affinity_mask);

// Create a new micro-program
uint32_t sys_spawn(const cap_token_t *code_cap,    // Capability to code region
                   const cap_token_t *data_cap,    // Capability to data region
                   uint64_t entry_point,           // Entry address within code_cap
                   priority_t priority,
                   const char *name);

// Terminate a micro-program
void sys_exit(int32_t exit_code);

// Wait for a child micro-program to terminate
int32_t sys_wait(uint32_t child_id);

// Get micro-program information
int sys_mprog_info(uint32_t microprog_id, mprog_info_t *out);
```

---

## 8. Inter-Processor Interrupts (IPI)

The scheduler uses x2APIC IPIs for cross-core coordination:

| IPI Vector | Purpose |
|-----------|---------|
| 0xF0 | **Reschedule** — wake an idle core to check its run queue |
| 0xF1 | **TLB shootdown** — invalidate specific TLB entries (rare in SASOS) |
| 0xF2 | **Halt** — stop a core (for shutdown/power management) |
| 0xF3 | **Panic** — freeze all cores during kernel panic |
| 0xFE | **Spurious** — x2APIC spurious interrupt vector |

```c
// Wake a specific core to schedule a newly-enqueued task
void scheduler_notify_core(uint32_t core_id) {
    x2apic_send_ipi(g_cpu_table[core_id].x2apic_id, IPI_RESCHEDULE, APIC_DELIVERY_FIXED);
}

// Send task to a specific core's run queue (cross-core enqueue)
void scheduler_migrate(microprogram_t *mprog, uint32_t target_core) {
    per_core_scheduler_t *target = &g_schedulers[target_core];
    spinlock_lock(&target->lock);
    scheduler_enqueue(target, mprog);
    spinlock_unlock(&target->lock);
    scheduler_notify_core(target_core);
}
```

---

## 9. Real-Time Scheduling Guarantees

For `PRIORITY_REALTIME` micro-programs (compositor, audio pipeline):

- **Deadline-based scheduling:** Each real-time µP declares a period and a WCET (Worst-Case Execution Time). The scheduler uses Earliest Deadline First (EDF) within the real-time priority class.
- **Bandwidth reservation:** Real-time µPs cannot consume more than 80% of a core's time in aggregate (prevents starvation).
- **Interrupt coalescing:** Hardware interrupts targeting a core with a running real-time µP are deferred to the next scheduling point (except NMI).

```c
typedef struct rt_params {
    uint64_t period_ns;      // Period in nanoseconds
    uint64_t wcet_ns;        // Worst-case execution time
    uint64_t deadline_ns;    // Relative deadline (usually == period)
} rt_params_t;

int sys_set_realtime(uint32_t microprog_id, const rt_params_t *params);
```

---

## 10. TSC Calibration

The scheduler relies on the TSC (Time Stamp Counter) for all timing. During boot, we calibrate the TSC frequency:

```c
// Method 1: CPUID leaf 0x15 (preferred, available on modern Intel/AMD)
// TSC frequency = (ECX * EBX) / EAX
bool tsc_calibrate_cpuid(uint64_t *freq_hz);

// Method 2: PIT-based calibration (fallback)
// Count TSC ticks over a known PIT interval
bool tsc_calibrate_pit(uint64_t *freq_hz);

// Method 3: HPET-based calibration (if PIT is unavailable)
bool tsc_calibrate_hpet(uint64_t *freq_hz);

// Invariant TSC check (required for reliable scheduling)
bool tsc_is_invariant(void) {
    uint32_t edx;
    __cpuid(0x80000007, NULL, NULL, NULL, &edx);
    return (edx >> 8) & 1;  // bit 8: invariant TSC
}
```

---

*Next: [04-STORAGE.md](./04-STORAGE.md) — NVMe Driver & Object Graph File System*
