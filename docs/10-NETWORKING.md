# 10 — TCP/IP Stack & Zero-Copy Networking

> **Subsystem:** Networking  
> **Owner:** Network team  
> **Dependencies:** PCIe NIC driver, IOMMU, capability system, IPC  
> **Related:** [07-DRIVERS.md](./07-DRIVERS.md), [08-IPC.md](./08-IPC.md), [02-MEMORY.md](./02-MEMORY.md)

---

## 1. Design Philosophy

Helios networking is built on three principles:

1. **Zero-copy end-to-end** — Packet data stays in a single buffer from NIC DMA to application consumption
2. **No file descriptors** — There are no sockets, no `fd`-based API. Networking uses capability-mediated shared buffers and IPC
3. **Userspace-assisted fast path** — The network stack runs as a privileged micro-program, not inside the kernel

---

## 2. Architecture

```
┌─────────────────────────────────────────────────────────┐
│  Application Micro-Programs                              │
│  ┌──────┐  ┌──────┐  ┌──────┐                           │
│  │HTTP  │  │DNS   │  │Shell │                           │
│  │client│  │resolvr│  │(curl)│                           │
│  └──┬───┘  └──┬───┘  └──┬───┘                           │
│     │         │         │                                │
│     ▼         ▼         ▼                                │
│  ┌──────────────────────────────────────────────┐        │
│  │  NETWORK STACK MICRO-PROGRAM (Ring 3)         │        │
│  │  ┌────────────────────────────────────────┐   │        │
│  │  │  Application Layer (HTTP, DNS, TLS)    │   │        │
│  │  ├────────────────────────────────────────┤   │        │
│  │  │  Transport Layer (TCP, UDP)            │   │        │
│  │  ├────────────────────────────────────────┤   │        │
│  │  │  Network Layer (IPv4, IPv6, ICMP, ARP) │   │        │
│  │  ├────────────────────────────────────────┤   │        │
│  │  │  Packet Buffer Manager (zero-copy)      │   │        │
│  │  └────────────────────┬───────────────────┘   │        │
│  └───────────────────────┬───────────────────────┘        │
│                          │                                │
│  ┌───────────────────────▼───────────────────────┐        │
│  │  NIC DRIVER MICRO-PROGRAM (Ring 3)             │        │
│  │  ┌──────────────────────────────────────────┐  │        │
│  │  │  Ring buffer management                   │  │        │
│  │  │  DMA descriptor programming               │  │        │
│  │  │  MSI-X interrupt handling                  │  │        │
│  │  └──────────────────────────────────────────┘  │        │
│  └───────────────────────┬───────────────────────┘        │
│                          │                                │
│  ┌───────────────────────▼───────────────────────┐        │
│  │  NIC HARDWARE (e.g. Intel i210/i225, Virtio)   │        │
│  └───────────────────────────────────────────────┘        │
└─────────────────────────────────────────────────────────┘
```

---

## 3. Packet Buffer Architecture

### 3.1 Shared Packet Pool

A large, pre-allocated pool of packet buffers is shared between the NIC driver, network stack, and applications via capabilities:

```c
#define PKT_BUF_SIZE        2048        // Per-buffer size (fits jumbo MTU header + data)
#define PKT_POOL_COUNT      16384       // Total buffers in pool
#define PKT_POOL_SIZE       (PKT_BUF_SIZE * PKT_POOL_COUNT)  // 32 MiB

typedef struct {
    uint8_t         data[PKT_BUF_SIZE];
} __attribute__((aligned(64))) pkt_buf_t;

typedef struct {
    pkt_buf_t      *buffers;            // Base of buffer pool (SASOS mapped)
    phys_addr_t     buffers_phys;       // Physical base (for DMA)
    uint64_t        pool_size;

    // Free list (lock-free MPSC queue)
    struct {
        _Atomic uint32_t head;
        _Atomic uint32_t tail;
        uint16_t         indices[PKT_POOL_COUNT];
    } free_list;

    cap_token_t     pool_cap;           // Capability for the entire pool
} pkt_pool_t;
```

### 3.2 Zero-Copy Packet Flow

```
RX Path (NIC → Application):
  1. NIC DMAs packet into pre-registered pkt_buf_t via IOMMU
  2. NIC driver receives MSI-X interrupt
  3. Driver derives a cap for the specific pkt_buf_t (read-only)
  4. Driver sends cap to network stack via IPC
  5. Network stack processes headers (in-place, no copy)
  6. Network stack derives a cap pointing to payload offset
  7. Network stack sends payload cap to application via IPC
  8. Application reads data directly from the buffer
  9. Application releases buffer → returned to free list

TX Path (Application → NIC):
  1. Application allocates pkt_buf_t from pool
  2. Application writes payload data
  3. Application sends cap + metadata to network stack
  4. Network stack prepends headers (in-place, buffer has headroom)
  5. Network stack sends cap to NIC driver
  6. NIC driver programs DMA descriptor with buffer physical address
  7. NIC transmits packet
  8. NIC signals completion → buffer returned to free list
```

---

## 4. Network Stack Implementation

### 4.1 Layer Structure

```c
// Packet descriptor (metadata, not the packet data itself)
typedef struct {
    uint16_t        buf_index;          // Index into pkt_pool
    uint16_t        data_offset;        // Offset to start of current layer's data
    uint16_t        data_len;           // Length of current layer's data
    uint16_t        total_len;          // Total packet length
    uint32_t        flags;              // PKT_FLAG_CHECKSUM_VALID, etc.

    // Parsed header offsets (filled during RX processing)
    uint16_t        l2_offset;          // Ethernet header
    uint16_t        l3_offset;          // IP header
    uint16_t        l4_offset;          // TCP/UDP header
    uint16_t        payload_offset;     // Application payload

    // Metadata
    uint32_t        src_ip;
    uint32_t        dst_ip;
    uint16_t        src_port;
    uint16_t        dst_port;
    uint8_t         protocol;           // IPPROTO_TCP, IPPROTO_UDP
    uint8_t         _pad[3];
} pkt_desc_t;
```

### 4.2 ARP

```c
// ARP cache (IP → MAC mapping)
typedef struct {
    uint32_t        ip_addr;
    uint8_t         mac_addr[6];
    uint16_t        state;          // ARP_INCOMPLETE, ARP_REACHABLE, ARP_STALE
    uint64_t        expiry_tsc;     // Cache expiration
} arp_entry_t;

#define ARP_CACHE_SIZE 256
extern arp_entry_t g_arp_cache[ARP_CACHE_SIZE];

int arp_resolve(uint32_t ip, uint8_t *mac_out);
void arp_handle_packet(pkt_desc_t *pkt);
```

### 4.3 IPv4 / IPv6

```c
// IP routing table entry
typedef struct {
    uint32_t    network;            // Network address
    uint32_t    netmask;            // Subnet mask
    uint32_t    gateway;            // Gateway IP (0 = direct)
    uint32_t    interface_id;       // NIC interface index
    uint32_t    metric;             // Route priority
} route_entry_t;

int ip_send(uint32_t dst_ip, uint8_t protocol, const void *payload, uint16_t len);
void ip_handle_packet(pkt_desc_t *pkt);
```

### 4.4 TCP

Full TCP implementation with:

```c
typedef enum {
    TCP_CLOSED, TCP_LISTEN, TCP_SYN_SENT, TCP_SYN_RECEIVED,
    TCP_ESTABLISHED, TCP_FIN_WAIT_1, TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT, TCP_CLOSING, TCP_LAST_ACK, TCP_TIME_WAIT,
} tcp_state_t;

typedef struct tcp_connection {
    tcp_state_t     state;

    // Local/remote endpoints
    uint32_t        local_ip;
    uint16_t        local_port;
    uint32_t        remote_ip;
    uint16_t        remote_port;

    // Sequence numbers
    uint32_t        snd_una;        // Send unacknowledged
    uint32_t        snd_nxt;        // Send next
    uint32_t        snd_wnd;        // Send window
    uint32_t        rcv_nxt;        // Receive next
    uint32_t        rcv_wnd;        // Receive window

    // Congestion control
    uint32_t        cwnd;           // Congestion window
    uint32_t        ssthresh;       // Slow start threshold
    uint32_t        rtt_us;         // Smoothed RTT in microseconds
    uint32_t        rto_us;         // Retransmission timeout

    // Buffers (zero-copy: these are capability pointers into pkt_pool)
    struct {
        pkt_desc_t  *ring;
        uint32_t     head, tail;
        uint32_t     capacity;
    } send_queue, recv_queue;

    // Retransmission
    struct {
        pkt_desc_t  *packets;
        uint64_t    *send_times;
        uint32_t     count;
    } retransmit_queue;

    // IPC integration
    uint64_t        notify_port;    // Port to notify on data arrival

    struct list_head list;
} tcp_connection_t;
```

### 4.5 UDP

```c
typedef struct udp_binding {
    uint32_t        local_ip;
    uint16_t        local_port;
    uint64_t        recv_port;      // IPC port for received datagrams
    cap_token_t     pool_cap;       // Capability to packet pool
    struct list_head list;
} udp_binding_t;
```

---

## 5. Network API (Syscalls)

There are no sockets. Applications interact with the network stack via IPC messages:

```c
// ── Connection-Oriented (TCP) ──

// Open a TCP connection
typedef struct {
    uint32_t    remote_ip;
    uint16_t    remote_port;
    uint64_t    notify_port;        // IPC port for events
} net_connect_request_t;

// Listen for incoming connections
typedef struct {
    uint16_t    local_port;
    uint32_t    backlog;
    uint64_t    accept_port;        // IPC port to receive new connections
} net_listen_request_t;

// Send data on a connection
typedef struct {
    uint32_t    conn_id;
    cap_token_t data_cap;           // Capability to data buffer
    uint32_t    data_len;
} net_send_request_t;

// ── Connectionless (UDP) ──

// Bind a UDP port
typedef struct {
    uint16_t    local_port;
    uint64_t    recv_port;          // IPC port for received datagrams
} net_udp_bind_request_t;

// Send a UDP datagram
typedef struct {
    uint32_t    remote_ip;
    uint16_t    remote_port;
    cap_token_t data_cap;
    uint32_t    data_len;
} net_udp_send_request_t;

// ── Configuration ──

// Set interface IP address
typedef struct {
    uint32_t    interface_id;
    uint32_t    ip_addr;
    uint32_t    netmask;
    uint32_t    gateway;
} net_ifconfig_request_t;

// DNS resolution (via network stack's built-in resolver)
typedef struct {
    char        hostname[256];
    uint64_t    reply_port;         // IPC port for the resolved address
} net_dns_request_t;
```

### 5.1 Convenience Syscall Wrappers

For common operations, thin syscall wrappers are provided in a user-space library:

```c
// High-level API (implemented as IPC to helios.network service)
int     net_connect(uint32_t ip, uint16_t port, net_conn_t *conn);
int     net_listen(uint16_t port, uint32_t backlog, net_listener_t *listener);
int     net_accept(net_listener_t *listener, net_conn_t *conn);
ssize_t net_send(net_conn_t *conn, const void *data, size_t len);
ssize_t net_recv(net_conn_t *conn, void *buf, size_t max_len);
void    net_close(net_conn_t *conn);

int     net_udp_open(uint16_t local_port, net_udp_t *udp);
ssize_t net_udp_sendto(net_udp_t *udp, uint32_t ip, uint16_t port,
                       const void *data, size_t len);
ssize_t net_udp_recvfrom(net_udp_t *udp, void *buf, size_t max_len,
                         uint32_t *src_ip, uint16_t *src_port);

int     net_resolve(const char *hostname, uint32_t *ip_out);
```

---

## 6. NIC Driver Interface

### 6.1 Supported NICs

| NIC | Interface | Notes |
|-----|-----------|-------|
| Virtio-net | Virtqueue | Primary development target (QEMU) |
| Intel i210/i225 | Intel IGC register set | Common 1 GbE / 2.5 GbE |
| Intel E1000e | E1000e register set | Very common in VMs |

### 6.2 Driver ↔ Network Stack Protocol

The NIC driver and network stack communicate via IPC:

```c
// Messages from NIC driver → Network stack
#define NIC_MSG_RX_PACKET       0x0001  // Received packet (carries pkt_desc_t + cap)
#define NIC_MSG_TX_COMPLETE     0x0002  // TX buffer completed (can be freed)
#define NIC_MSG_LINK_UP         0x0003  // Link state change
#define NIC_MSG_LINK_DOWN       0x0004

// Messages from Network stack → NIC driver
#define NIC_MSG_TX_PACKET       0x0010  // Transmit packet (carries pkt_desc_t + cap)
#define NIC_MSG_SET_PROMISC     0x0011  // Enable/disable promiscuous mode
#define NIC_MSG_SET_MULTICAST   0x0012  // Configure multicast filters
```

---

## 7. DHCP Client

A built-in DHCP client micro-program handles IP configuration:

```
Boot sequence:
  1. NIC driver initializes, link comes up
  2. DHCP client sends DHCPDISCOVER (broadcast)
  3. DHCP server replies with DHCPOFFER
  4. Client sends DHCPREQUEST
  5. Server sends DHCPACK
  6. Client configures interface via net_ifconfig IPC
  7. Client sets default gateway via net_route_add IPC
  8. Emits SIG_NETWORK_UP signal
```

---

## 8. Performance Targets

| Metric | Target |
|--------|--------|
| Packet RX → application delivery (zero-copy) | < 5 µs |
| TCP throughput (single connection, 1 GbE) | > 900 Mbps |
| UDP throughput (small packets, 64 B) | > 1 Mpps |
| Connection establishment (TCP handshake) | < 500 µs LAN |
| DNS resolution (cached) | < 1 µs |

---

*Next: [11-SHELL.md](./11-SHELL.md) — Graph-Query Shell & AI-Assisted Command Pipeline*
