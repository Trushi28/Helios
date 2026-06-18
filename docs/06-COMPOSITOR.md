# 06 — GPU Vertex-Matrix Compositor

> **Subsystem:** Display & UI  
> **Owner:** Graphics team  
> **Dependencies:** GPU driver, PCIe, PMM, SASOS shared buffers  
> **Related:** [02-MEMORY.md](./02-MEMORY.md), [07-DRIVERS.md](./07-DRIVERS.md), [15-USB.md](./15-USB.md)

---

## 1. Design Philosophy

Traditional desktop compositors (Wayland, X11, DWM) treat windows as **opaque rectangles** shuffled by a window manager. The compositor blits pre-rendered bitmaps onto a screen buffer. Customization means changing themes — colors, borders, fonts.

Helios takes a radically different approach: **the entire UI is a mathematical function of a reactive data matrix, rendered as GPU geometry.**

There are no windows, widgets, or toolkits. There are:

1. **Data Matrix** — A global, reactive, shared-memory state buffer
2. **Structural Shaders** — User-defined math that maps data → vertex positions
3. **Render Pipeline** — A tight GPU loop that draws the computed geometry every frame

The UI is not drawn. It is **computed**.

---

## 2. Architecture

```
┌────────────────────────────────────────────────────────────────┐
│                    MICRO-PROGRAMS (User Space)                  │
│                                                                │
│  ┌──────┐  ┌──────┐  ┌──────┐  ┌──────────────────────────┐   │
│  │Shell │  │Editor│  │Stats │  │ Structural Shader (user) │   │
│  │      │  │      │  │      │  │ - layout_shader.hlsl     │   │
│  │writes│  │writes│  │writes│  │ - style_shader.hlsl      │   │
│  │data  │  │data  │  │data  │  │                          │   │
│  └──┬───┘  └──┬───┘  └──┬───┘  └────────────┬─────────────┘   │
│     │         │         │                    │                  │
│     ▼         ▼         ▼                    ▼                  │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │              DATA MATRIX (Shared SASOS Region)           │   │
│  │  ┌─────────┐ ┌─────────┐ ┌─────────┐ ┌──────────────┐  │   │
│  │  │Cell[0,0]│ │Cell[0,1]│ │Cell[1,0]│ │Cell[N,M]     │  │   │
│  │  │type:text│ │type:text│ │type:gfx │ │type:metric   │  │   │
│  │  │content: │ │content: │ │content: │ │value: 3.14   │  │   │
│  │  │"$ ls"   │ │"fn main"│ │<bitmap> │ │label:"CPU"   │  │   │
│  │  │dirty:1  │ │dirty:0  │ │dirty:1  │ │dirty:1       │  │   │
│  │  └─────────┘ └─────────┘ └─────────┘ └──────────────┘  │   │
│  └──────────────────────────┬───────────────────────────────┘   │
│                              │                                  │
│  ┌───────────────────────────▼──────────────────────────────┐   │
│  │              COMPOSITOR ENGINE (Kernel Micro-Program)     │   │
│  │                                                           │   │
│  │  1. Scan data matrix for dirty cells                      │   │
│  │  2. Run structural shader → compute vertex positions      │   │
│  │  3. Rasterize text (GPU glyph atlas)                      │   │
│  │  4. Build GPU command buffer                              │   │
│  │  5. Submit to GPU                                         │   │
│  │  6. VSync / present                                       │   │
│  └───────────────────────────┬──────────────────────────────┘   │
│                              │                                  │
│  ┌───────────────────────────▼──────────────────────────────┐   │
│  │              GPU HARDWARE                                 │   │
│  │  ┌──────────────┐  ┌──────────────┐  ┌────────────────┐  │   │
│  │  │Vertex Shader │  │Fragment      │  │ Framebuffer    │  │   │
│  │  │(structural   │  │Shader        │  │ (scanout to    │  │   │
│  │  │ layout)      │  │(color/style) │  │  display)      │  │   │
│  │  └──────────────┘  └──────────────┘  └────────────────┘  │   │
│  └──────────────────────────────────────────────────────────┘   │
└────────────────────────────────────────────────────────────────┘
```

---

## 3. Data Matrix

### 3.1 Structure

The data matrix is a 2D grid of **cells** stored in a shared SASOS region. Each micro-program that contributes to the UI writes its output to assigned cells.

```c
typedef enum {
    CELL_TYPE_EMPTY     = 0,
    CELL_TYPE_TEXT      = 1,    // UTF-8 text content (terminal output, code)
    CELL_TYPE_GLYPH     = 2,    // Single rendered glyph
    CELL_TYPE_GRAPHIC   = 3,    // Bitmap/vector graphic reference
    CELL_TYPE_METRIC    = 4,    // Numeric value (CPU%, memory, etc.)
    CELL_TYPE_CANVAS    = 5,    // Raw pixel buffer (for custom rendering)
    CELL_TYPE_CURSOR    = 6,    // Input cursor position
} cell_type_t;

typedef struct {
    cell_type_t     type;
    uint32_t        flags;          // CELL_DIRTY, CELL_FOCUSED, CELL_SELECTED
    uint32_t        owner_mprog;    // Which micro-program owns this cell

    union {
        struct {
            char        text[256];      // UTF-8 text content
            uint32_t    text_len;
            uint32_t    fg_color;       // 0xAARRGGBB
            uint32_t    bg_color;
            uint16_t    font_id;        // Index into font atlas
            uint16_t    font_size;      // In 1/64th point units
            uint32_t    style;          // BOLD, ITALIC, UNDERLINE, etc.
        } text;

        struct {
            float       value;
            float       min, max;
            uint32_t    color;
            char        label[32];
        } metric;

        struct {
            uint64_t    texture_id;     // GPU texture reference
            uint16_t    width, height;
        } graphic;

        struct {
            uint64_t    buffer_offset;  // Offset in GPU vertex buffer
            uint32_t    vertex_count;
        } canvas;
    };

    // Layout hints (used by structural shader as inputs)
    float           weight;         // Relative size weight (flex-grow equivalent)
    float           min_width;      // Minimum width in normalized coords
    float           min_height;
    float           aspect_ratio;   // 0 = unconstrained
    uint32_t        z_order;        // Stacking order
} matrix_cell_t;

// The data matrix itself
typedef struct {
    matrix_cell_t  *cells;          // Flat array, row-major
    uint32_t        rows;
    uint32_t        cols;
    uint32_t        capacity;       // Max cells
    uint64_t        generation;     // Incremented on every mutation
    spinlock_t      lock;           // Fine-grained: per-cell atomics for dirty flags
} data_matrix_t;
```

### 3.2 Matrix Regions

Each micro-program claims a **region** of the matrix via capability:

```c
// Request a region in the data matrix
// Returns a capability token for the assigned cells
cap_token_t sys_ui_claim_region(uint32_t rows, uint32_t cols,
                                 float weight, const char *label);

// Write to a cell (must hold capability for that region)
int sys_ui_write_cell(cap_token_t *region_cap,
                      uint32_t row, uint32_t col,
                      const matrix_cell_t *cell);

// Mark cells as dirty (triggers re-render on next frame)
void sys_ui_invalidate(cap_token_t *region_cap,
                       uint32_t row, uint32_t col,
                       uint32_t width, uint32_t height);

// Release a region
void sys_ui_release_region(cap_token_t *region_cap);
```

### 3.3 Dirty Tracking

Each cell has a `CELL_DIRTY` flag. The compositor only re-processes dirty cells each frame, enabling partial updates:

```c
#define CELL_DIRTY      (1 << 0)
#define CELL_FOCUSED    (1 << 1)
#define CELL_SELECTED   (1 << 2)
#define CELL_HIDDEN     (1 << 3)
#define CELL_ANIMATED   (1 << 4)
```

---

## 4. Structural Shaders

### 4.1 Concept

A structural shader is a user-defined function that transforms the abstract data matrix into concrete GPU vertex positions. It answers the question: **"Given N cells with these properties, where does each cell appear on screen?"**

```
Input:  matrix_cell_t cells[]  +  screen dimensions  +  user parameters
Output: vertex_quad_t quads[]  (position + UV + color for each cell)
```

### 4.2 Shader Types

| Shader Stage | Input | Output | Purpose |
|-------------|-------|--------|---------|
| **Layout Shader** | Cell metadata (weights, counts) | Quad positions (x, y, w, h) | Determines spatial arrangement |
| **Style Shader** | Cell content + quad positions | Color, opacity, border, effects | Visual appearance |
| **Animation Shader** | Previous frame state + time delta | Position/style interpolation | Smooth transitions |

### 4.3 Built-In Layout Presets

Helios ships with several structural shader presets:

#### Tiling Layout (default)
```
┌──────────┬──────────┬──────────┐
│          │          │          │
│  Cell 0  │  Cell 1  │  Cell 2  │
│          │          │          │
├──────────┼──────────┴──────────┤
│          │                     │
│  Cell 3  │      Cell 4         │
│          │                     │
└──────────┴─────────────────────┘
```

#### Stack Layout
```
┌─────────────────────────────────┐
│            Cell 0               │
├─────────────────────────────────┤
│            Cell 1               │
├─────────────────────────────────┤
│            Cell 2               │
└─────────────────────────────────┘
```

#### Cylindrical Ring Layout (dynamic)
```
        ╭─── Cell 0 ───╮
    ╭───╯               ╰───╮
  Cell 4                   Cell 1
    ╰───╮               ╭───╯
        ╰─── Cell 3 ───╯
              Cell 2
              (back)
```

#### Perspective Grid
```
    ╱ Cell 0 ╱ Cell 1 ╱ Cell 2 ╱
   ╱────────╱────────╱────────╱
  ╱ Cell 3 ╱ Cell 4 ╱ Cell 5 ╱
 ╱────────╱────────╱────────╱
```

### 4.4 Custom Shader Example

A user writes a shader that arranges cells in a radial pattern based on their activity level:

```glsl
// layout_radial.vert — Custom structural shader
// Runs on GPU as a vertex shader

layout(binding = 0) uniform LayoutUniforms {
    float screen_width;
    float screen_height;
    float time;           // Seconds since boot
    uint  cell_count;
    float focus_index;    // Currently focused cell
};

struct CellMeta {
    float weight;
    float activity;       // 0.0–1.0, computed from dirty frequency
    uint  type;
    uint  flags;
};

layout(binding = 1) buffer CellData {
    CellMeta cells[];
};

// Output: computed quad for this cell
layout(location = 0) out vec2 out_position;
layout(location = 1) out vec2 out_size;
layout(location = 2) out float out_opacity;

void main() {
    uint idx = gl_VertexIndex;
    if (idx >= cell_count) return;

    float angle = (float(idx) / float(cell_count)) * 2.0 * 3.14159;
    float radius = 0.3 + cells[idx].activity * 0.15;  // More active = further out

    // Focused cell moves to center
    if (idx == uint(focus_index)) {
        out_position = vec2(0.0, 0.0);
        out_size = vec2(0.5, 0.5);
        out_opacity = 1.0;
    } else {
        float wobble = sin(time * 2.0 + angle) * 0.02;  // Subtle animation
        out_position = vec2(
            cos(angle) * (radius + wobble),
            sin(angle) * (radius + wobble)
        );
        out_size = vec2(0.2, 0.15) * cells[idx].weight;
        out_opacity = 0.7 + cells[idx].activity * 0.3;
    }
}
```

### 4.5 Shader Hot-Reload

Structural shaders are stored as objects in the graph store. Editing a shader object triggers:

1. Graph mutation event → compositor receives notification
2. Compositor compiles new shader (GPU shader compiler, or SPIRV from pre-compiled)
3. Next frame uses new layout
4. Previous frame's layout interpolates smoothly to new positions

```c
// Set active structural shader for the compositor
int sys_ui_set_shader(object_id_t layout_shader_oid,
                      object_id_t style_shader_oid);

// Set shader parameters (uniforms accessible from the shader)
int sys_ui_set_param(const char *name, float value);
int sys_ui_set_param_vec2(const char *name, float x, float y);
int sys_ui_set_param_color(const char *name, uint32_t rgba);
```

---

## 5. Text Rendering

### 5.1 GPU Glyph Atlas

Text is rendered using a **GPU glyph atlas** — a pre-rendered texture containing all required glyphs:

```c
typedef struct {
    uint64_t        texture_id;     // GPU texture handle
    uint32_t        atlas_width;    // Texture width (e.g., 4096)
    uint32_t        atlas_height;   // Texture height
    uint32_t        glyph_count;

    struct {
        uint32_t    codepoint;      // Unicode codepoint
        uint16_t    x, y;           // Position in atlas texture
        uint16_t    w, h;           // Glyph dimensions in pixels
        int16_t     bearing_x;      // Horizontal bearing
        int16_t     bearing_y;      // Vertical bearing
        uint16_t    advance;        // Horizontal advance width
    } glyphs[8192];                 // Up to 8K glyphs in atlas

    struct hash_table codepoint_index;  // codepoint → glyph index
} glyph_atlas_t;
```

### 5.2 Font Loading

Fonts are stored as objects in the graph store. During compositor init:

1. Load font objects (TrueType/OpenType)
2. Rasterize glyphs at required sizes using a built-in font rasterizer
3. Pack glyphs into GPU atlas texture
4. Build codepoint → atlas-position lookup table

```c
// Register a font from the object store
int sys_ui_load_font(object_id_t font_oid, uint16_t *font_id);

// Built-in system font (always available, embedded in kernel)
#define FONT_SYSTEM_MONO   0    // Monospace (for terminal)
#define FONT_SYSTEM_SANS   1    // Sans-serif (for UI labels)
```

### 5.3 Text Rendering Pipeline

```
Text cell → glyph lookup → generate quad per glyph → vertex buffer → GPU
```

Each text character becomes a textured quad referencing the glyph atlas. The fragment shader samples the atlas texture with sub-pixel anti-aliasing.

---

## 6. GPU Driver Model

### 6.1 Early Boot (GOP Framebuffer)

Before the GPU driver is initialized, the compositor uses the **UEFI GOP linear framebuffer** for basic text output (boot messages, panic screens):

```c
typedef struct {
    volatile uint32_t   *fb_base;       // Mapped framebuffer
    uint32_t             width, height;
    uint32_t             pitch;         // Bytes per scanline
    uint32_t             bpp;
} gop_framebuffer_t;

// Simple pixel write (no GPU acceleration)
void gop_put_pixel(gop_framebuffer_t *fb, uint32_t x, uint32_t y, uint32_t color);
void gop_draw_rect(gop_framebuffer_t *fb, uint32_t x, uint32_t y,
                   uint32_t w, uint32_t h, uint32_t color);
void gop_draw_char(gop_framebuffer_t *fb, uint32_t x, uint32_t y,
                   char c, uint32_t fg, uint32_t bg);
```

### 6.2 Full GPU Driver

The full GPU driver interfaces with the GPU via PCIe MMIO and provides:

```c
typedef struct gpu_device {
    pcie_device_t          *pci_dev;
    volatile void          *mmio_base;       // BAR0 mapped registers
    uint64_t                vram_base_phys;  // Video RAM physical address
    uint64_t                vram_size;

    // Command submission
    struct {
        volatile uint32_t  *ring_base;       // Command ring buffer
        uint32_t            ring_size;
        uint32_t            write_ptr;
        uint32_t            read_ptr;
        volatile uint32_t  *doorbell;
    } cmd_ring;

    // Display controller
    struct {
        uint32_t    width, height;
        uint32_t    refresh_rate;
        uint64_t    scanout_addr;   // Framebuffer address in VRAM
    } display;

    // Shader compiler
    struct {
        void       *compiler_ctx;   // SPIR-V → GPU ISA compiler
    } shader;
} gpu_device_t;
```

### 6.3 Supported GPU Targets

| Target | Status | Interface |
|--------|--------|-----------|
| QEMU virtio-gpu | Primary dev target | Virtio protocol over PCI |
| QEMU QXL / VGA | Fallback for testing | Standard VGA + MMIO extensions |
| Intel HD/UHD/Xe | Phase 4+ | i915-compatible register interface |
| AMD RDNA | Phase 4+ | AMDGPU register interface |
| Software rasterizer | Always available | CPU fallback (slow) |

For initial development, we target **virtio-gpu** under QEMU/KVM, which provides:
- Vulkan 1.2 support via virglrenderer
- 3D acceleration with GPU command queues
- Display configuration via virtio protocol

---

## 7. Render Loop

### 7.1 Frame Pipeline

```c
_Noreturn void compositor_main(void) {
    gpu_device_t *gpu = gpu_get_primary();
    data_matrix_t *matrix = ui_get_matrix();
    glyph_atlas_t *atlas = ui_get_glyph_atlas();

    while (true) {
        uint64_t frame_start = rdtsc();

        // 1. Scan for dirty cells
        uint32_t dirty_count = matrix_scan_dirty(matrix, dirty_list, MAX_DIRTY);

        if (dirty_count > 0 || animation_active()) {
            // 2. Run layout structural shader (GPU vertex shader)
            gpu_dispatch_layout_shader(matrix, dirty_list, dirty_count);

            // 3. Generate text vertex data (quads referencing glyph atlas)
            text_renderer_update(matrix, atlas, dirty_list, dirty_count);

            // 4. Build GPU command buffer
            gpu_cmd_buffer_t *cmd = gpu_cmd_alloc();
            gpu_cmd_begin_render_pass(cmd, gpu->display.scanout_addr);
            gpu_cmd_bind_pipeline(cmd, &compositor_pipeline);
            gpu_cmd_bind_vertex_buffer(cmd, &vertex_buffer);
            gpu_cmd_bind_texture(cmd, 0, atlas->texture_id);
            gpu_cmd_draw(cmd, total_vertex_count, 0);

            // 5. Draw overlays (cursor, selection, notifications)
            overlay_render(cmd);

            gpu_cmd_end_render_pass(cmd);

            // 6. Submit and present
            gpu_submit(gpu, cmd);
            gpu_present(gpu);    // Swap buffers / wait for VSync

            // 7. Clear dirty flags
            matrix_clear_dirty(matrix, dirty_list, dirty_count);
        } else {
            // Nothing changed — sleep until next dirty event or VSync
            sys_sleep(1000000);  // 1 ms idle poll (will be replaced with event-driven)
        }

        // Frame timing
        uint64_t frame_end = rdtsc();
        frame_time_ns = tsc_to_ns(frame_end - frame_start);
        // Target: 16.67 ms (60 FPS) or 6.94 ms (144 FPS)
    }
}
```

### 7.2 Animation System

Animations are driven by the compositor's clock:

```c
typedef struct animation {
    uint32_t        cell_row, cell_col;
    float           start_value, end_value;
    float           duration_ms;
    float           elapsed_ms;
    enum { EASE_LINEAR, EASE_IN_OUT, EASE_BOUNCE, EASE_SPRING } easing;
    enum { ANIM_POSITION_X, ANIM_POSITION_Y, ANIM_OPACITY,
           ANIM_SCALE, ANIM_COLOR, ANIM_ROTATION } property;
} animation_t;

// Queue an animation
int sys_ui_animate(uint32_t row, uint32_t col,
                   uint32_t property, float target, float duration_ms,
                   uint32_t easing);
```

---

## 8. Input Handling

### 8.1 Input Events

Input from USB HID devices (keyboard, mouse) flows through the compositor:

```c
typedef enum {
    INPUT_KEY_PRESS,
    INPUT_KEY_RELEASE,
    INPUT_MOUSE_MOVE,
    INPUT_MOUSE_BUTTON,
    INPUT_MOUSE_SCROLL,
    INPUT_TOUCH,
} input_type_t;

typedef struct {
    input_type_t    type;
    uint64_t        timestamp_ns;
    union {
        struct { uint32_t keycode; uint32_t modifiers; } key;
        struct { int32_t x, y; int32_t dx, dy; } mouse_move;
        struct { uint32_t button; bool pressed; } mouse_button;
        struct { int32_t dx, dy; } scroll;
    };
} input_event_t;
```

### 8.2 Focus Model

The compositor maintains a **focus stack**:

```c
// Focus a specific cell region (receives keyboard input)
int sys_ui_focus(cap_token_t *region_cap);

// Register an input handler for a region
int sys_ui_on_input(cap_token_t *region_cap,
                    uint64_t ipc_port);  // IPC port to receive input events
```

Input events are routed to the focused region's owning micro-program via IPC.

---

## 9. Display Configuration

```c
// Query available display modes
int sys_display_modes(display_mode_t *modes, uint32_t *count);

// Set display mode
int sys_display_set_mode(uint32_t width, uint32_t height, uint32_t refresh_hz);

// Multi-monitor support (future)
int sys_display_enumerate(display_info_t *displays, uint32_t *count);
```

---

## 10. Performance Targets

| Metric | Target |
|--------|--------|
| Frame render time (idle desktop) | < 2 ms |
| Frame render time (active terminal + code editor) | < 8 ms |
| Input-to-pixel latency | < 16 ms (1 frame at 60 Hz) |
| Text rendering throughput | > 100K glyphs/frame |
| Layout shader execution | < 0.5 ms for 64 cells |
| Memory usage (compositor) | < 64 MiB (excluding VRAM) |

---

*Next: [07-DRIVERS.md](./07-DRIVERS.md) — Driver Isolation & PCIe Enumeration*
