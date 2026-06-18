# 15 — xHCI USB 3.x Host Controller & HID Input

> **Subsystem:** USB  
> **Owner:** Driver team  
> **Dependencies:** PCIe, IOMMU, capability system, IPC  
> **Related:** [07-DRIVERS.md](./07-DRIVERS.md), [06-COMPOSITOR.md](./06-COMPOSITOR.md), [14-INTERRUPTS.md](./14-INTERRUPTS.md)

---

## 1. Design Goals

| Goal | Rationale |
|------|-----------|
| xHCI only | USB 3.x host controller — no UHCI/OHCI/EHCI legacy |
| Keyboard + mouse priority | Essential input for initial OS interaction |
| Driver isolation | xHCI driver runs as a user-space micro-program |
| Interrupt-driven | MSI-X for event notification, no polling |

---

## 2. xHCI Architecture Overview

```
┌──────────────────────────────────────────────────────┐
│  xHCI Host Controller (PCIe device)                   │
│                                                      │
│  ┌──────────────────┐  ┌──────────────────────────┐  │
│  │  Capability Regs  │  │  Operational Registers    │  │
│  │  (BAR0 + 0x00)    │  │  (BAR0 + cap_length)      │  │
│  └──────────────────┘  └──────────────────────────┘  │
│                                                      │
│  ┌──────────────────────────────────────────────────┐ │
│  │  Runtime Registers (BAR0 + RTSOFF)               │ │
│  │  └─ Interrupter[0..N] (Event Ring management)     │ │
│  └──────────────────────────────────────────────────┘ │
│                                                      │
│  ┌──────────────────────────────────────────────────┐ │
│  │  Doorbell Array (BAR0 + DBOFF)                    │ │
│  │  └─ Doorbell[0] = Host Controller                  │ │
│  │  └─ Doorbell[1..N] = Device Slots                  │ │
│  └──────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────┘
```

### 2.1 Key Concepts

| Concept | Description |
|---------|-------------|
| **Transfer Ring** | Per-endpoint circular buffer of Transfer TRBs (Transfer Request Blocks) |
| **Event Ring** | Ring where the controller posts completion events |
| **Command Ring** | Ring for host → controller commands (enable slot, configure endpoint, etc.) |
| **TRB** | Transfer Request Block — 16-byte command/data descriptor |
| **Device Slot** | Logical slot representing a connected USB device |
| **Device Context** | Controller-managed data structure describing device state |
| **DCBAA** | Device Context Base Address Array — maps slot IDs to device contexts |

---

## 3. xHCI Initialization

```c
void xhci_init(pcie_device_t *pdev) {
    // 1. Map BAR0 (xHCI registers)
    volatile xhci_cap_regs_t *cap = mmio_map_bar(pdev, 0, xhci_mprog_id);
    volatile xhci_op_regs_t  *op  = (void *)cap + cap->cap_length;
    volatile xhci_rt_regs_t  *rt  = (void *)cap + cap->rts_offset;
    volatile uint32_t        *db  = (void *)cap + cap->db_offset;

    // 2. Halt controller (USBCMD.RS = 0)
    op->usbcmd &= ~XHCI_CMD_RUN;
    while (!(op->usbsts & XHCI_STS_HCH)) cpu_pause();

    // 3. Reset controller (USBCMD.HCRST = 1)
    op->usbcmd |= XHCI_CMD_HCRST;
    while (op->usbcmd & XHCI_CMD_HCRST) cpu_pause();
    while (op->usbsts & XHCI_STS_CNR) cpu_pause();

    // 4. Read capabilities
    uint32_t max_slots = (cap->hcs_params1 >> 0) & 0xFF;
    uint32_t max_interrupters = (cap->hcs_params1 >> 8) & 0x7FF;
    uint32_t max_ports = (cap->hcs_params1 >> 24) & 0xFF;

    // 5. Set Max Device Slots Enabled
    op->config = max_slots;

    // 6. Allocate DCBAA (Device Context Base Address Array)
    phys_addr_t dcbaa = pmm_alloc_pages(0, ALLOC_CONTIGUOUS | ALLOC_ZERO);
    op->dcbaap = dcbaa;

    // 7. Allocate Command Ring
    xhci_ring_init(&g_cmd_ring, 256);
    op->crcr = g_cmd_ring.phys | XHCI_CRCR_RCS;

    // 8. Allocate Event Ring for Interrupter 0
    xhci_event_ring_init(&g_event_ring, 256);
    rt->interrupters[0].erst_size = 1;
    rt->interrupters[0].erst_base = g_event_ring.erst_phys;
    rt->interrupters[0].erdp = g_event_ring.phys;

    // 9. Enable interrupter
    rt->interrupters[0].iman = XHCI_IMAN_IE;

    // 10. Setup MSI-X
    msix_configure(pdev, 0, xhci_irq_vector, target_apic_id);

    // 11. Start controller (USBCMD.RS = 1, INTE = 1)
    op->usbcmd |= XHCI_CMD_RUN | XHCI_CMD_INTE;

    // 12. Scan ports for connected devices
    for (uint32_t port = 0; port < max_ports; port++) {
        if (op->port_regs[port].portsc & XHCI_PORTSC_CCS) {
            xhci_port_reset(op, port);
            xhci_enumerate_device(port);
        }
    }
}
```

---

## 4. TRB (Transfer Request Block)

```c
typedef struct {
    uint64_t    param;          // Parameter (address, inline data, etc.)
    uint32_t    status;         // Transfer length, completion code
    uint32_t    control;        // TRB type, flags, cycle bit
} __attribute__((packed)) xhci_trb_t;

_Static_assert(sizeof(xhci_trb_t) == 16, "TRB must be 16 bytes");

// TRB Types
#define TRB_TYPE_NORMAL         1
#define TRB_TYPE_SETUP_STAGE    2
#define TRB_TYPE_DATA_STAGE     3
#define TRB_TYPE_STATUS_STAGE   4
#define TRB_TYPE_LINK           6
#define TRB_TYPE_EVENT_DATA     7
#define TRB_TYPE_ENABLE_SLOT    9
#define TRB_TYPE_DISABLE_SLOT   10
#define TRB_TYPE_ADDRESS_DEV    11
#define TRB_TYPE_CONFIG_EP      12
#define TRB_TYPE_TRANSFER_EVENT 32
#define TRB_TYPE_CMD_COMPLETE   33
#define TRB_TYPE_PORT_STATUS    34
```

---

## 5. Device Enumeration

When a USB device is detected on a port:

```
1. Port status change → Port Status Change Event TRB
2. Reset the port (write PORTSC.PR = 1)
3. Enable Slot command → get slot ID
4. Read device descriptor (control transfer to endpoint 0)
5. Set Address command
6. Read configuration descriptor
7. Read interface descriptors
8. Match interface class/subclass/protocol to driver:
   - Class 0x03, Subclass 0x01, Protocol 0x01 → HID Keyboard
   - Class 0x03, Subclass 0x01, Protocol 0x02 → HID Mouse
9. Configure Endpoint command (set up interrupt IN endpoint)
10. Schedule periodic interrupt transfers for HID polling
```

```c
typedef struct {
    uint32_t    slot_id;
    uint16_t    vendor_id;
    uint16_t    product_id;
    uint8_t     class;
    uint8_t     subclass;
    uint8_t     protocol;
    uint8_t     speed;          // USB_SPEED_LOW, _FULL, _HIGH, _SUPER
    uint8_t     port;
    uint8_t     address;
    char        product_string[64];
    char        manufacturer_string[64];

    // Endpoints
    struct {
        uint8_t     address;    // Endpoint address (bit 7 = direction)
        uint8_t     type;       // Control, Bulk, Interrupt, Isochronous
        uint16_t    max_packet;
        uint8_t     interval;   // Polling interval (for interrupt endpoints)
        xhci_ring_t transfer_ring;
    } endpoints[16];
    uint8_t     endpoint_count;
} usb_device_t;

#define USB_MAX_DEVICES 127
extern usb_device_t g_usb_devices[USB_MAX_DEVICES];
extern uint32_t     g_usb_device_count;
```

---

## 6. HID (Human Interface Device) Driver

### 6.1 HID Boot Protocol

For initial keyboard/mouse support, we use the **HID Boot Protocol** (simple, fixed-format reports):

```c
// Boot protocol keyboard report (8 bytes)
typedef struct {
    uint8_t     modifiers;      // Bit flags: Ctrl, Shift, Alt, GUI
    uint8_t     _reserved;
    uint8_t     keycodes[6];    // Up to 6 simultaneous key presses (USB HID keycodes)
} __attribute__((packed)) hid_keyboard_report_t;

// Boot protocol mouse report (3+ bytes)
typedef struct {
    uint8_t     buttons;        // Bit flags: left, right, middle
    int8_t      x_delta;        // X movement (-127 to +127)
    int8_t      y_delta;        // Y movement
} __attribute__((packed)) hid_mouse_report_t;
```

### 6.2 Keycode Translation

USB HID keycodes are translated to Helios scancodes:

```c
// USB HID keycode → Helios keycode mapping table
// Based on USB HID Usage Tables (HID Usage Page 0x07)
static const uint32_t hid_to_helios[256] = {
    [0x04] = KEY_A, [0x05] = KEY_B, [0x06] = KEY_C, [0x07] = KEY_D,
    [0x08] = KEY_E, [0x09] = KEY_F, [0x0A] = KEY_G, [0x0B] = KEY_H,
    [0x0C] = KEY_I, [0x0D] = KEY_J, [0x0E] = KEY_K, [0x0F] = KEY_L,
    [0x10] = KEY_M, [0x11] = KEY_N, [0x12] = KEY_O, [0x13] = KEY_P,
    [0x14] = KEY_Q, [0x15] = KEY_R, [0x16] = KEY_S, [0x17] = KEY_T,
    [0x18] = KEY_U, [0x19] = KEY_V, [0x1A] = KEY_W, [0x1B] = KEY_X,
    [0x1C] = KEY_Y, [0x1D] = KEY_Z,
    [0x1E] = KEY_1, [0x1F] = KEY_2, [0x20] = KEY_3, [0x21] = KEY_4,
    [0x22] = KEY_5, [0x23] = KEY_6, [0x24] = KEY_7, [0x25] = KEY_8,
    [0x26] = KEY_9, [0x27] = KEY_0,
    [0x28] = KEY_ENTER, [0x29] = KEY_ESCAPE, [0x2A] = KEY_BACKSPACE,
    [0x2B] = KEY_TAB,   [0x2C] = KEY_SPACE,
    // ... full table
};
```

### 6.3 Input Event Delivery

HID reports are translated to `input_event_t` and delivered to the compositor via IPC:

```c
void hid_keyboard_handle_report(hid_keyboard_report_t *report,
                                hid_keyboard_report_t *prev) {
    // Detect newly pressed keys (in report but not in prev)
    for (int i = 0; i < 6; i++) {
        if (report->keycodes[i] == 0) continue;
        bool was_pressed = false;
        for (int j = 0; j < 6; j++) {
            if (prev->keycodes[j] == report->keycodes[i]) {
                was_pressed = true;
                break;
            }
        }
        if (!was_pressed) {
            input_event_t event = {
                .type = INPUT_KEY_PRESS,
                .timestamp_ns = tsc_to_ns(rdtsc()),
                .key.keycode = hid_to_helios[report->keycodes[i]],
                .key.modifiers = report->modifiers,
            };
            ipc_send(compositor_input_port, &event, sizeof(event));
        }
    }

    // Detect released keys (in prev but not in report)
    for (int i = 0; i < 6; i++) {
        if (prev->keycodes[i] == 0) continue;
        bool still_pressed = false;
        for (int j = 0; j < 6; j++) {
            if (report->keycodes[j] == prev->keycodes[i]) {
                still_pressed = true;
                break;
            }
        }
        if (!still_pressed) {
            input_event_t event = {
                .type = INPUT_KEY_RELEASE,
                .timestamp_ns = tsc_to_ns(rdtsc()),
                .key.keycode = hid_to_helios[prev->keycodes[i]],
                .key.modifiers = report->modifiers,
            };
            ipc_send(compositor_input_port, &event, sizeof(event));
        }
    }

    *prev = *report;
}
```

---

## 7. USB Hotplug

Port status change events trigger device enumeration/removal:

```c
void xhci_port_status_change(uint32_t port) {
    uint32_t portsc = op->port_regs[port].portsc;

    if (portsc & XHCI_PORTSC_CSC) {
        // Connection Status Change
        if (portsc & XHCI_PORTSC_CCS) {
            // Device connected
            xhci_port_reset(op, port);
            xhci_enumerate_device(port);
            sys_signal_emit(SIG_DEVICE_ATTACHED, &port, sizeof(port));
        } else {
            // Device disconnected
            xhci_remove_device(port);
            sys_signal_emit(SIG_DEVICE_DETACHED, &port, sizeof(port));
        }
        // Acknowledge the change
        op->port_regs[port].portsc = portsc | XHCI_PORTSC_CSC;
    }
}
```

---

## 8. USB Subsystem API

```c
// List connected USB devices
int sys_usb_enumerate(usb_device_summary_t *out, uint32_t *count);

// Get device details
int sys_usb_device_info(uint32_t slot_id, usb_device_info_t *out);

// Subscribe to USB hotplug events
int sys_usb_watch(uint64_t ipc_port);
```

---

*Next: [16-AUDIO.md](./16-AUDIO.md) — Intel HDA & Low-Latency Audio Pipeline*
