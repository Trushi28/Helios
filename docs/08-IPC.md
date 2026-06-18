# 08 — Zero-Copy Capability-Mediated IPC

> **Subsystem:** Inter-Process Communication  
> **Owner:** Kernel team  
> **Dependencies:** Capability system, SASOS, scheduler  
> **Related:** [02-MEMORY.md](./02-MEMORY.md), [03-SCHEDULER.md](./03-SCHEDULER.md), [09-SECURITY.md](./09-SECURITY.md)

---

## 1. The SASOS IPC Advantage

In traditional operating systems, IPC is expensive because every process has an isolated address space. Sending data requires copying bytes across the kernel boundary — `write()` into a pipe, `sendmsg()` through a socket, `copy_to_user()`/`copy_from_user()` for every shared buffer.

In Helios's Single Address Space, **all micro-programs can see the same virtual addresses**. The only barrier is the capability system. IPC reduces to:

1. Derive a sub-capability from your data buffer
2. Hand the sub-capability to the target micro-program
3. The target reads the data directly — **zero copies, zero kernel involvement for data transfer**

The kernel only mediates the **control plane** (routing, synchronization, notification). The **data plane** is pure shared memory.

---

## 2. IPC Primitives

Helios provides three IPC mechanisms, ordered by increasing complexity:

| Mechanism | Latency | Use Case |
|-----------|---------|----------|
| **Capability Transfer** | ~10 ns | Bulk data sharing (buffers, objects) |
| **Ports & Messages** | ~100 ns | Structured control messages, request/reply |
| **Signal Graph** | ~50 ns | Event notification, pub/sub |

---

## 3. Capability Transfer (Data Plane)

### 3.1 Direct Pointer Sharing

The simplest form of IPC: one micro-program derives a sub-capability and transfers it to another.

```c
// Micro-program A: share a read-only view of a data buffer with B
cap_token_t shared = cap_derive(&my_buffer_cap,
                                 my_buffer_cap.base,
                                 my_buffer_cap.length,
                                 CAP_PERM_READ);  // Read-only

// Transfer to B (kernel validates and registers the transfer)
sys_cap_transfer(&shared, target_mprog_id_B);

// Micro-program B can now directly dereference the pointer
// const char *data = (const char *)shared.base;
// No copies. No syscalls for data access.
```

### 3.2 Transfer Semantics

| Transfer Mode | Description |
|--------------|-------------|
| **Copy** | Recipient gets a new capability; sender retains original |
| **Move** | Sender's capability is revoked; recipient gets exclusive access |
| **Lend** | Sender retains ownership; recipient's capability auto-revokes on sender death |

```c
typedef enum {
    CAP_XFER_COPY,      // Both parties retain access
    CAP_XFER_MOVE,      // Sender loses access
    CAP_XFER_LEND,      // Temporary grant, auto-revoked
} cap_transfer_mode_t;

int sys_cap_transfer(cap_token_t *cap, uint32_t target_mprog_id,
                     cap_transfer_mode_t mode);
```

---

## 4. Ports & Messages (Control Plane)

### 4.1 Port Model

Each micro-program can create **ports** — named endpoints for receiving structured messages:

```c
typedef struct ipc_port {
    uint64_t            port_id;
    uint32_t            owner_mprog_id;
    char                name[32];       // Optional human-readable name

    // Message queue (ring buffer)
    struct {
        ipc_message_t  *buffer;
        uint32_t        capacity;       // Power of 2
        uint32_t        head;           // Producer index (atomic)
        uint32_t        tail;           // Consumer index (atomic)
    } queue;

    // Wait queue for blocked receivers
    struct list_head    waiters;

    // Capabilities attached to this port
    cap_token_t         queue_cap;      // Capability for the queue memory

    spinlock_t          lock;
} ipc_port_t;
```

### 4.2 Message Structure

```c
#define IPC_MSG_MAX_INLINE  192     // Max inline payload bytes
#define IPC_MSG_MAX_CAPS    4       // Max capability attachments per message

typedef struct {
    uint32_t        msg_type;       // Application-defined message type
    uint32_t        flags;          // IPC_FLAG_REPLY_EXPECTED, etc.
    uint32_t        sender_mprog;   // Filled by kernel
    uint32_t        sender_port;    // Reply port (for request/reply pattern)
    uint64_t        timestamp;      // TSC at send time

    // Inline payload (small messages, no allocation needed)
    uint8_t         inline_data[IPC_MSG_MAX_INLINE];
    uint32_t        inline_len;

    // Capability attachments (for sharing large data)
    cap_token_t     caps[IPC_MSG_MAX_CAPS];
    uint32_t        cap_count;
} ipc_message_t;
```

### 4.3 Send / Receive Syscalls

```c
// Create a port
uint64_t sys_port_create(const char *name, uint32_t queue_capacity);

// Destroy a port
void sys_port_destroy(uint64_t port_id);

// Send a message to a port (non-blocking if queue has space)
int sys_ipc_send(uint64_t target_port, const ipc_message_t *msg);

// Receive a message (blocks if queue is empty)
int sys_ipc_recv(uint64_t my_port, ipc_message_t *msg_out);

// Try to receive (non-blocking, returns -EAGAIN if empty)
int sys_ipc_try_recv(uint64_t my_port, ipc_message_t *msg_out);

// Send and wait for reply (synchronous call pattern)
int sys_ipc_call(uint64_t target_port, const ipc_message_t *request,
                 ipc_message_t *reply_out, uint64_t timeout_ns);

// Reply to a received message
int sys_ipc_reply(uint64_t reply_port, const ipc_message_t *reply);

// Wait on multiple ports (select/poll equivalent)
int sys_ipc_wait_any(uint64_t *port_ids, uint32_t port_count,
                     ipc_message_t *msg_out, uint64_t timeout_ns);
```

### 4.4 Request/Reply Pattern

The most common IPC pattern: a client sends a request and blocks for a response.

```
Client µP                           Server µP
  │                                    │
  │  sys_ipc_call(server_port, req)──▶│  sys_ipc_recv(my_port, &req)
  │  (blocked, waiting for reply)      │
  │                                    │  ... process request ...
  │                                    │
  │  ◀── sys_ipc_reply(reply_port) ── │
  │  (unblocked, reply received)       │
```

Implementation detail: `sys_ipc_call` atomically:
1. Creates a temporary reply port
2. Fills `msg.sender_port` with the reply port
3. Sends the message
4. Blocks on the reply port
5. Destroys the reply port after receiving the reply

---

## 5. Signal Graph (Event Notification)

### 5.1 Concept

The signal graph is a lightweight pub/sub mechanism for event-driven programming. Micro-programs **subscribe** to named signals and get notified when events occur.

```c
typedef uint64_t signal_id_t;

// Well-known system signals
#define SIG_DEVICE_ATTACHED     0x0001
#define SIG_DEVICE_DETACHED     0x0002
#define SIG_OBJECT_CREATED      0x0003
#define SIG_OBJECT_MODIFIED     0x0004
#define SIG_DISPLAY_RESIZE      0x0005
#define SIG_POWER_STATE_CHANGE  0x0006
#define SIG_NETWORK_UP          0x0007
#define SIG_NETWORK_DOWN        0x0008
#define SIG_INFER_COMPLETE      0x0009
#define SIG_TIMER_EXPIRED       0x000A
#define SIG_MICROPROG_EXIT      0x000B

// Custom user-defined signals start at 0x1000
#define SIG_USER_BASE           0x1000
```

### 5.2 API

```c
// Create a custom signal
signal_id_t sys_signal_create(const char *name);

// Subscribe to a signal (notifications delivered to an IPC port)
int sys_signal_subscribe(signal_id_t signal, uint64_t port_id);

// Unsubscribe
int sys_signal_unsubscribe(signal_id_t signal, uint64_t port_id);

// Emit a signal (broadcast to all subscribers)
int sys_signal_emit(signal_id_t signal, const void *data, uint32_t data_len);
```

### 5.3 Signal Delivery

Signals are delivered as `ipc_message_t` with `msg_type = IPC_MSG_TYPE_SIGNAL`:

```c
#define IPC_MSG_TYPE_SIGNAL  0xFFFF0001

// Signal payload (in inline_data)
typedef struct {
    signal_id_t     signal;
    uint32_t        emitter_mprog;
    uint64_t        timestamp;
    uint8_t         data[160];      // Signal-specific payload
    uint32_t        data_len;
} signal_payload_t;
```

---

## 6. Named Service Registry

Micro-programs can register named services, allowing clients to discover them by name rather than hardcoded port IDs:

```c
// Register a named service (makes a port discoverable)
int sys_service_register(const char *service_name, uint64_t port_id);

// Lookup a service by name (returns the port ID)
int sys_service_lookup(const char *service_name, uint64_t *port_id_out);

// List all registered services
int sys_service_list(service_entry_t *out, uint32_t *count);
```

### 6.1 Well-Known Services

| Service Name | Provider | Purpose |
|-------------|----------|---------|
| `helios.storage` | Object store engine | Object CRUD, graph queries |
| `helios.compositor` | Compositor | UI region allocation, rendering |
| `helios.infer` | Inference scheduler | sys_infer request routing |
| `helios.network` | Network stack | Socket-less networking |
| `helios.usb` | USB subsystem | Device enumeration, HID events |
| `helios.audio` | Audio subsystem | Audio stream management |
| `helios.power` | Power manager | Suspend, resume, shutdown |

---

## 7. Performance

### 7.1 IPC Latency Breakdown

```
Capability Transfer (data plane):
  cap_derive():           ~5 ns   (compute HMAC)
  sys_cap_transfer():     ~10 ns  (register in cap table)
  Data access by recipient: 0 ns  (direct pointer dereference)
  TOTAL:                  ~15 ns

Port Message (control plane):
  sys_ipc_send():
    Validate sender cap:   ~5 ns
    Copy inline payload:   ~20 ns  (192 bytes)
    Ring buffer enqueue:   ~10 ns  (atomic head advance)
    Wake blocked receiver: ~50 ns  (scheduler IPI if cross-core)
  TOTAL send:             ~85 ns

  sys_ipc_recv():
    Ring buffer dequeue:   ~10 ns
    Copy to user buffer:   ~20 ns
  TOTAL recv:             ~30 ns

  TOTAL roundtrip:        ~115 ns

vs. Traditional OS:
  Linux pipe write+read:  ~2000 ns
  Linux Unix socket:      ~4000 ns
  Linux shared memory + futex: ~500 ns
```

### 7.2 Throughput

| Metric | Target |
|--------|--------|
| Small messages (192 B, single core) | > 10 M msg/sec |
| Small messages (cross-core) | > 5 M msg/sec |
| Bulk data transfer (cap sharing) | Memory bandwidth limited (~40 GB/s DDR4) |

---

## 8. Deadlock Prevention

In a microkernel with synchronous IPC, deadlocks can occur when two micro-programs call each other. Helios prevents this with:

1. **Timeout on all blocking calls** — `sys_ipc_call` has a mandatory timeout parameter
2. **Deadlock detection** — the kernel tracks the "waiting-for" graph and aborts circular dependencies
3. **Async-first design** — most services use `sys_ipc_send` + `sys_ipc_recv` (async) rather than `sys_ipc_call` (sync)

```c
// Kernel deadlock detector
bool ipc_detect_deadlock(uint32_t waiter_mprog, uint32_t waitee_mprog) {
    // Walk the waiting-for graph: if waitee is (transitively) waiting for waiter,
    // we have a cycle → deadlock
    uint32_t current = waitee_mprog;
    for (int depth = 0; depth < 64; depth++) {
        uint32_t waiting_for = scheduler_get_blocked_on(current);
        if (waiting_for == 0) return false;         // Not waiting on anyone
        if (waiting_for == waiter_mprog) return true; // Cycle detected!
        current = waiting_for;
    }
    return false;  // No cycle within depth limit
}
```

---

*Next: [09-SECURITY.md](./09-SECURITY.md) — Capability System & Secure Boot Chain*
