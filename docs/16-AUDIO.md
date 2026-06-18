# 16 — Intel HDA & Low-Latency Audio Pipeline

> **Subsystem:** Audio  
> **Owner:** Driver team  
> **Dependencies:** PCIe, IOMMU, capability system, IPC, scheduler  
> **Related:** [07-DRIVERS.md](./07-DRIVERS.md), [03-SCHEDULER.md](./03-SCHEDULER.md), [14-INTERRUPTS.md](./14-INTERRUPTS.md)

---

## 1. Design Goals

| Goal | Rationale |
|------|-----------|
| Intel HDA (High Definition Audio) | Universal audio controller on modern x86 hardware |
| Low latency | Target < 10 ms end-to-end audio latency |
| Zero-copy audio buffers | DMA directly from shared SASOS memory |
| Real-time scheduling | Audio micro-program runs at `PRIORITY_REALTIME` |
| Mixing | Kernel-level audio mixer for multiple sources |

---

## 2. Intel HDA Overview

### 2.1 Architecture

```
┌──────────────────────────────────────────────────────┐
│  HDA Controller (PCIe device, usually on chipset)     │
│                                                      │
│  ┌─────────────────┐  ┌──────────────────────────┐  │
│  │  Global Regs     │  │  Stream Descriptors       │  │
│  │  GCAP, GCTL,     │  │  SD[0..N]: DMA engine     │  │
│  │  WAKEEN, etc.    │  │  per audio stream          │  │
│  └─────────────────┘  └──────────────────────────┘  │
│                                                      │
│  ┌──────────────────────────────────────────────────┐ │
│  │  CORB (Command Outbound Ring Buffer)              │ │
│  │  Host → codec commands                            │ │
│  ├──────────────────────────────────────────────────┤ │
│  │  RIRB (Response Inbound Ring Buffer)              │ │
│  │  Codec → host responses                           │ │
│  └──────────────────────────────────────────────────┘ │
│                                                      │
│  ┌─────────────────────────────┐                     │
│  │  BDL (Buffer Descriptor     │                     │
│  │  List) — points to DMA       │                     │
│  │  audio data buffers          │                     │
│  └─────────────────────────────┘                     │
└──────────────────────────────────────────────────────┘
        │
        │ HD-Audio Link
        ▼
┌──────────────────────────────────────────────────────┐
│  HDA Codec (on HD-Audio link, e.g. Realtek ALC)      │
│                                                      │
│  ┌──────────────┐  ┌──────────────┐  ┌───────────┐  │
│  │  Audio Func   │  │  DAC Widget  │  │  Pin       │  │
│  │  Group        │  │  (digital→   │  │  Widget    │  │
│  │               │  │   analog)    │  │  (output)  │  │
│  └──────────────┘  └──────────────┘  └───────────┘  │
└──────────────────────────────────────────────────────┘
```

### 2.2 Key Registers (BAR0 MMIO)

```c
typedef struct {
    // Global registers
    uint16_t    gcap;           // Global Capabilities
    uint8_t     vmin;           // Minor version
    uint8_t     vmaj;           // Major version
    uint16_t    outpay;         // Output Payload Capability
    uint16_t    inpay;          // Input Payload Capability
    uint32_t    gctl;           // Global Control (CRST, FCNTRL, UNSOL)
    uint16_t    wakeen;         // Wake Enable
    uint16_t    statests;       // State Change Status (codec detection)
    uint16_t    gsts;           // Global Status
    uint8_t     _pad0[6];
    uint16_t    outstrmpay;     // Output Stream Payload Capability
    uint16_t    instrmpay;      // Input Stream Payload Capability
    uint8_t     _pad1[4];

    // CORB registers
    uint32_t    corblbase;      // CORB Lower Base Address
    uint32_t    corbubase;      // CORB Upper Base Address
    uint16_t    corbwp;         // CORB Write Pointer
    uint16_t    corbrp;         // CORB Read Pointer
    uint8_t     corbctl;        // CORB Control (run bit)
    uint8_t     corbsts;        // CORB Status
    uint8_t     corbsize;       // CORB Size

    uint8_t     _pad2;

    // RIRB registers
    uint32_t    rirblbase;
    uint32_t    rirbubase;
    uint16_t    rirbwp;
    uint16_t    rintcnt;        // Response Interrupt Count
    uint8_t     rirbctl;
    uint8_t     rirbsts;
    uint8_t     rirbsize;

    uint8_t     _pad3;

    // DMA Position Buffer
    uint32_t    dpiblbase;
    uint32_t    dpibubase;

    uint8_t     _pad4[8];

    // Stream Descriptors (SD0, SD1, ... at offset 0x80+)
} __attribute__((packed)) hda_regs_t;

// Stream Descriptor registers (per stream, at 0x80 + N*0x20)
typedef struct {
    uint16_t    ctl_sts;        // Control + Status
    uint8_t     ctl2;           // Control byte 2 (stream number, etc.)
    uint8_t     _pad0;
    uint32_t    lpib;           // Link Position in Buffer (read-only)
    uint32_t    cbl;            // Cyclic Buffer Length (total DMA bytes)
    uint16_t    lvi;            // Last Valid Index in BDL
    uint16_t    _pad1;
    uint16_t    fifos;          // FIFO Size
    uint16_t    fmt;            // Stream Format (sample rate, bits, channels)
    uint32_t    _pad2;
    uint32_t    bdlpl;          // BDL Pointer Lower
    uint32_t    bdlpu;          // BDL Pointer Upper
} __attribute__((packed)) hda_sd_regs_t;
```

---

## 3. HDA Controller Initialization

```c
void hda_init(pcie_device_t *pdev) {
    volatile hda_regs_t *regs = mmio_map_bar(pdev, 0, audio_mprog_id);

    // 1. Reset controller
    regs->gctl &= ~HDA_GCTL_CRST;
    while (regs->gctl & HDA_GCTL_CRST) cpu_pause();
    regs->gctl |= HDA_GCTL_CRST;
    while (!(regs->gctl & HDA_GCTL_CRST)) cpu_pause();

    // 2. Wait for codecs to enumerate (~521 µs)
    sys_sleep(1000000);  // 1 ms

    // 3. Check STATESTS for detected codecs
    uint16_t codecs = regs->statests;
    for (int i = 0; i < 15; i++) {
        if (codecs & (1 << i)) {
            serial_printf("HDA: Codec found at address %d\n", i);
            hda_codec_init(regs, i);
        }
    }

    // 4. Set up CORB (Command Outbound Ring Buffer)
    phys_addr_t corb = pmm_alloc_pages(0, ALLOC_CONTIGUOUS | ALLOC_ZERO);
    regs->corblbase = (uint32_t)corb;
    regs->corbubase = (uint32_t)(corb >> 32);
    regs->corbsize = 0x02;  // 256 entries
    regs->corbctl = HDA_CORB_RUN;

    // 5. Set up RIRB (Response Inbound Ring Buffer)
    phys_addr_t rirb = pmm_alloc_pages(0, ALLOC_CONTIGUOUS | ALLOC_ZERO);
    regs->rirblbase = (uint32_t)rirb;
    regs->rirbubase = (uint32_t)(rirb >> 32);
    regs->rirbsize = 0x02;  // 256 entries
    regs->rintcnt = 1;      // Interrupt after every response
    regs->rirbctl = HDA_RIRB_RUN | HDA_RIRB_IRQ_EN;

    // 6. Configure MSI-X
    msix_configure(pdev, 0, hda_irq_vector, target_apic_id);

    // 7. Enable interrupts
    regs->gctl |= HDA_GCTL_UNSOL;  // Enable unsolicited responses
}
```

---

## 4. Codec Communication

### 4.1 Verb/Response Protocol

Commands to the codec are encoded as 32-bit "verbs" sent via CORB:

```c
// HDA verb format: [Codec Addr:4][Node ID:8][Verb:20]
#define HDA_VERB(codec, node, verb) \
    (((uint32_t)(codec) << 28) | ((uint32_t)(node) << 20) | (verb))

// Common verbs
#define HDA_VERB_GET_PARAM         0xF0000    // Get Parameter
#define HDA_VERB_SET_STREAM_FMT    0x20000    // Set Stream Format
#define HDA_VERB_SET_PIN_CTRL      0x70700    // Set Pin Widget Control
#define HDA_VERB_SET_POWER_STATE   0x70500    // Set Power State
#define HDA_VERB_SET_CONV_CTRL     0x70600    // Set Converter Control
#define HDA_VERB_SET_AMP_GAIN      0x30000    // Set Amplifier Gain/Mute
#define HDA_VERB_SET_EAPD          0x70C00    // Set EAPD/BTL Enable

// Parameter IDs (for GET_PARAM)
#define HDA_PARAM_VENDOR_ID        0x00
#define HDA_PARAM_REVISION_ID      0x02
#define HDA_PARAM_NODE_COUNT       0x04
#define HDA_PARAM_FN_GROUP_TYPE    0x05
#define HDA_PARAM_AUDIO_CAPS       0x09
#define HDA_PARAM_PIN_CAPS         0x0C
#define HDA_PARAM_CONN_LIST_LEN    0x0E
#define HDA_PARAM_OUT_AMP_CAPS     0x12

void hda_send_verb(volatile hda_regs_t *regs, uint32_t verb) {
    uint16_t wp = regs->corbwp;
    wp = (wp + 1) % 256;
    g_corb_buffer[wp] = verb;
    regs->corbwp = wp;
}

uint32_t hda_get_response(volatile hda_regs_t *regs) {
    // Wait for RIRB entry
    while (!(regs->rirbsts & HDA_RIRB_RESPONSE_READY)) cpu_pause();
    uint16_t rp = (regs->rirbwp + 1) % 256;
    uint64_t response = g_rirb_buffer[rp];
    regs->rirbsts = HDA_RIRB_RESPONSE_READY;  // Acknowledge
    return (uint32_t)response;  // Lower 32 bits = response data
}
```

### 4.2 Codec Topology Discovery

```c
typedef struct {
    uint8_t     codec_addr;
    uint16_t    vendor_id;
    uint16_t    device_id;

    // Widget tree
    struct {
        uint8_t     nid;        // Node ID
        uint8_t     type;       // Audio Output, Audio Input, Pin Complex, Mixer, etc.
        uint32_t    caps;       // Widget capabilities
        uint8_t     connections[16];
        uint8_t     conn_count;
    } widgets[64];
    uint32_t    widget_count;

    // Discovered paths
    struct {
        uint8_t     dac_nid;    // DAC widget NID
        uint8_t     pin_nid;    // Output pin NID
        uint8_t     pin_config; // Pin configuration (jack type, location)
    } output_paths[8];
    uint32_t    output_path_count;
} hda_codec_t;
```

---

## 5. Audio Stream Setup

### 5.1 Buffer Descriptor List (BDL)

Each stream uses a cyclic buffer described by a BDL:

```c
typedef struct {
    uint64_t    address;        // Physical address of audio data buffer
    uint32_t    length;         // Buffer length in bytes
    uint32_t    ioc;            // Interrupt on Completion flag
} __attribute__((packed)) hda_bdl_entry_t;

#define HDA_BDL_ENTRIES     32
#define HDA_BUFFER_SIZE     4096    // Per-fragment size (4 KiB = ~23 ms at 44.1 kHz stereo 16-bit)
```

### 5.2 Stream Format

```c
// Stream format register encoding
// Bits [14]    = Type (0 = PCM)
// Bits [13:11] = Base Rate (0 = 48 kHz, 1 = 44.1 kHz)
// Bits [10:8]  = Rate Multiplier
// Bits [7:4]   = Rate Divider
// Bits [6:4]   = Bits per Sample (001 = 16-bit, 100 = 32-bit)
// Bits [3:0]   = Number of channels - 1

#define HDA_FMT_48KHZ_16BIT_STEREO  0x0011  // 48 kHz, 16-bit, 2 channels
#define HDA_FMT_44K1_16BIT_STEREO   0x4011  // 44.1 kHz, 16-bit, 2 channels
#define HDA_FMT_48KHZ_24BIT_STEREO  0x0031  // 48 kHz, 24-bit, 2 channels
```

### 5.3 Starting a Playback Stream

```c
void hda_start_output_stream(volatile hda_regs_t *regs, uint8_t stream_idx,
                              hda_codec_t *codec, uint8_t output_path_idx) {
    volatile hda_sd_regs_t *sd = hda_get_sd(regs, stream_idx);
    uint8_t dac_nid = codec->output_paths[output_path_idx].dac_nid;
    uint8_t pin_nid = codec->output_paths[output_path_idx].pin_nid;
    uint8_t stream_tag = stream_idx + 1;

    // 1. Allocate DMA buffers and BDL
    hda_bdl_entry_t *bdl = alloc_bdl();
    for (int i = 0; i < HDA_BDL_ENTRIES; i++) {
        bdl[i].address = pmm_alloc_pages(0, ALLOC_CONTIGUOUS);
        bdl[i].length = HDA_BUFFER_SIZE;
        bdl[i].ioc = (i == HDA_BDL_ENTRIES - 1) ? 1 : 0;
    }

    // 2. Configure stream descriptor
    sd->ctl_sts = 0;    // Stop stream
    sd->fmt = HDA_FMT_48KHZ_16BIT_STEREO;
    sd->cbl = HDA_BDL_ENTRIES * HDA_BUFFER_SIZE;
    sd->lvi = HDA_BDL_ENTRIES - 1;
    sd->bdlpl = (uint32_t)(uintptr_t)bdl;
    sd->bdlpu = (uint32_t)((uintptr_t)bdl >> 32);

    // 3. Set stream tag + channel in SD control
    sd->ctl2 = (stream_tag << 4);

    // 4. Configure codec: set DAC stream/channel
    hda_send_verb(regs, HDA_VERB(codec->codec_addr, dac_nid,
                  HDA_VERB_SET_CONV_CTRL | (stream_tag << 4)));

    // 5. Set DAC format
    hda_send_verb(regs, HDA_VERB(codec->codec_addr, dac_nid,
                  HDA_VERB_SET_STREAM_FMT | HDA_FMT_48KHZ_16BIT_STEREO));

    // 6. Enable output pin
    hda_send_verb(regs, HDA_VERB(codec->codec_addr, pin_nid,
                  HDA_VERB_SET_PIN_CTRL | 0x40));  // OUT enable

    // 7. Set amplifier gain (unmute, reasonable volume)
    hda_send_verb(regs, HDA_VERB(codec->codec_addr, dac_nid,
                  HDA_VERB_SET_AMP_GAIN | 0xB040));  // Output, left+right, gain=0x40

    // 8. Start stream (SD CTL: RUN bit)
    sd->ctl_sts |= HDA_SD_CTL_RUN | HDA_SD_CTL_IOCE | HDA_SD_CTL_FEIE;
}
```

---

## 6. Audio Mixer

### 6.1 Software Mixer

Multiple micro-programs can submit audio streams. The audio service mixes them:

```c
typedef struct {
    uint32_t        source_mprog_id;
    cap_token_t     buffer_cap;         // Shared audio buffer
    float           volume;             // 0.0 – 1.0
    float           pan;                // -1.0 (left) to +1.0 (right)
    bool            active;
} audio_source_t;

#define MAX_AUDIO_SOURCES 16
extern audio_source_t g_audio_sources[MAX_AUDIO_SOURCES];

void audio_mix(int16_t *output, uint32_t sample_count) {
    memset(output, 0, sample_count * sizeof(int16_t) * 2);  // Stereo

    for (int s = 0; s < MAX_AUDIO_SOURCES; s++) {
        if (!g_audio_sources[s].active) continue;

        int16_t *src = (int16_t *)g_audio_sources[s].buffer_cap.base;
        float vol = g_audio_sources[s].volume;

        for (uint32_t i = 0; i < sample_count * 2; i++) {
            int32_t mixed = output[i] + (int32_t)(src[i] * vol);
            // Clamp to int16 range
            if (mixed > 32767) mixed = 32767;
            if (mixed < -32768) mixed = -32768;
            output[i] = (int16_t)mixed;
        }
    }
}
```

---

## 7. Audio API

```c
// Open an audio output stream
int sys_audio_open(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample,
                   audio_stream_t *stream_out);

// Write audio samples to the stream
int sys_audio_write(audio_stream_t *stream, const void *samples, uint32_t byte_count);

// Set volume
int sys_audio_set_volume(audio_stream_t *stream, float volume);

// Close the stream
void sys_audio_close(audio_stream_t *stream);

// System-level audio control
int sys_audio_master_volume(float volume);
int sys_audio_mute(bool mute);
```

---

## 8. Performance Targets

| Metric | Target |
|--------|--------|
| Audio latency (buffer → speaker) | < 10 ms |
| DMA buffer fragment size | 4 KiB (≈ 23 ms at 44.1/16/stereo) |
| Mixer overhead per frame | < 100 µs |
| Maximum simultaneous streams | 16 |
| Supported sample rates | 44.1 kHz, 48 kHz, 96 kHz, 192 kHz |
| Supported bit depths | 16-bit, 24-bit, 32-bit |

---

*Next: [17-ROADMAP.md](./17-ROADMAP.md) — Phased Development Milestones*
