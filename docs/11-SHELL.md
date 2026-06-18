# 11 — Graph-Query Shell & AI-Assisted Command Pipeline

> **Subsystem:** User Interface / Shell  
> **Owner:** UX team  
> **Dependencies:** Compositor, object graph store, sys_infer, IPC  
> **Related:** [04-STORAGE.md](./04-STORAGE.md), [05-INTELLIGENCE.md](./05-INTELLIGENCE.md), [06-COMPOSITOR.md](./06-COMPOSITOR.md)

---

## 1. Design Philosophy

The Helios shell is not a text command interpreter in the Unix tradition. It is a **graph query interface** with native intelligence integration. Instead of navigating directory trees and piping text between programs, you:

1. **Query the object graph** to find data by relationships, tags, and semantic similarity
2. **Compose micro-program pipelines** that operate on typed objects, not byte streams
3. **Get AI-assisted completions** and suggestions at every interaction point

There are no paths like `/home/user/docs/report.txt`. There are graph queries like:

```
@authored_by(me) @tagged(report) @modified_after(2025-01-01)
```

---

## 2. Shell Architecture

```
┌───────────────────────────────────────────────────────────┐
│  SHELL MICRO-PROGRAM                                      │
│                                                           │
│  ┌────────────────────────────────────────────────────┐   │
│  │  Input Layer                                       │   │
│  │  ├─ Line editor (cursor, history, selection)       │   │
│  │  ├─ AI completion engine (sys_infer integration)   │   │
│  │  └─ Syntax highlighter                             │   │
│  ├────────────────────────────────────────────────────┤   │
│  │  Parser                                            │   │
│  │  ├─ Graph query parser                             │   │
│  │  ├─ Pipeline parser (A | B | C)                    │   │
│  │  └─ Expression evaluator                           │   │
│  ├────────────────────────────────────────────────────┤   │
│  │  Executor                                          │   │
│  │  ├─ Graph query engine (sys_query_* syscalls)      │   │
│  │  ├─ Micro-program launcher (sys_spawn)             │   │
│  │  ├─ Pipeline orchestrator (IPC wiring)             │   │
│  │  └─ Built-in commands                              │   │
│  ├────────────────────────────────────────────────────┤   │
│  │  Output Layer                                      │   │
│  │  ├─ Structured output formatter                    │   │
│  │  ├─ Data matrix writer (compositor integration)    │   │
│  │  └─ Object renderer (type-aware display)           │   │
│  └────────────────────────────────────────────────────┘   │
│                                                           │
│  Communicates via IPC with:                               │
│  ├─ helios.storage    (graph queries)                     │
│  ├─ helios.compositor (UI rendering)                      │
│  ├─ helios.infer      (AI completions)                    │
│  └─ helios.network    (network operations)                │
└───────────────────────────────────────────────────────────┘
```

---

## 3. Query Language

### 3.1 Syntax Overview

```
<query>     ::= <selector> { "|" <command> }*
<selector>  ::= <filter> { <filter> }*
<filter>    ::= "@" <predicate> "(" <args> ")"
              | "#" <tag>
              | ":" <type>
              | <literal_string>

<command>    ::= <name> { <arg> }*
```

### 3.2 Query Examples

```bash
# Find all C source objects tagged with "kernel"
:source #kernel #c

# Find objects authored by the current user, modified today
@authored_by(me) @modified_after(today)

# Semantic search: find code related to "memory allocation"
?? "memory allocation"

# Find objects that depend on the PMM module
@depends_on(:executable #pmm)

# Find the 5 largest objects in the "helios" project
@child_of(#helios) | sort size desc | head 5

# Trace the dependency graph of main.c
@label("main.c") | deps --recursive

# Find all snapshots from last week
:snapshot @created_after(7d_ago) | list --detail
```

### 3.3 Predicates

| Predicate | Description |
|-----------|-------------|
| `@label(pattern)` | Match vertex label (glob patterns) |
| `@type(vertex_type)` | Match vertex type |
| `@tagged(tag)` | Has a TAGGED edge to a tag vertex |
| `@authored_by(user)` | Has AUTHORED_BY edge |
| `@child_of(query)` | Has CHILD_OF edge to matching vertex |
| `@depends_on(query)` | Has DEPENDS_ON edge to matching vertex |
| `@modified_after(date)` | Created transaction after date |
| `@modified_before(date)` | Created transaction before date |
| `@size_gt(bytes)` | Object data size greater than |
| `@size_lt(bytes)` | Object data size less than |
| `@content_matches(regex)` | Content matches regex (scans object data) |
| `@semantic(query)` | Semantic similarity search via sys_infer |

### 3.4 Shorthand Notation

| Shorthand | Expansion |
|-----------|-----------|
| `#tag` | `@tagged(tag)` |
| `:type` | `@type(type)` |
| `"text"` | `@label("*text*")` |
| `??query` | `@semantic(query)` |

---

## 4. Pipeline System

### 4.1 Typed Pipelines

Unlike Unix pipes which pass unstructured byte streams, Helios pipelines pass **typed object streams**:

```
query → filter → transform → output
  ↓        ↓         ↓          ↓
vertices  vertices  vertices   display
```

Each pipeline stage is a micro-program that receives objects via IPC and emits objects via IPC.

### 4.2 Built-In Pipeline Commands

| Command | Input | Output | Description |
|---------|-------|--------|-------------|
| `list` | vertices | text | Display vertex metadata |
| `cat` | vertices | object data | Output object contents |
| `head N` | vertices | vertices (truncated) | First N results |
| `tail N` | vertices | vertices (truncated) | Last N results |
| `sort field [asc\|desc]` | vertices | vertices (sorted) | Sort by field |
| `filter expr` | vertices | vertices (filtered) | Additional filtering |
| `count` | vertices | number | Count results |
| `sum field` | vertices | number | Sum numeric field |
| `diff a b` | 2 objects | diff output | Content diff between objects |
| `edit` | vertex | vertex (modified) | Open object in editor |
| `tag name` | vertices | vertices | Add tag to objects |
| `untag name` | vertices | vertices | Remove tag |
| `link type target` | vertices | edges | Create edges |
| `unlink type target` | vertices | (none) | Remove edges |
| `exec` | vertices (executable) | output | Execute micro-programs |
| `store` | data stream | vertex | Store data as new object |
| `export path` | vertices | (side effect) | Export to external media |
| `summarize` | vertices | text | AI-generated summary |

### 4.3 Pipeline Execution

```c
// Pipeline stage descriptor
typedef struct {
    char            command[64];
    char            args[256];
    uint32_t        mprog_id;       // Spawned micro-program ID
    uint64_t        input_port;     // IPC port for receiving objects
    uint64_t        output_port;    // IPC port for sending objects
} pipeline_stage_t;

// Build and execute a pipeline
void pipeline_execute(const char *pipeline_text) {
    // 1. Parse pipeline stages
    pipeline_stage_t stages[MAX_PIPELINE_STAGES];
    uint32_t stage_count = pipeline_parse(pipeline_text, stages);

    // 2. Create IPC ports and wire them together
    for (uint32_t i = 0; i < stage_count; i++) {
        stages[i].input_port = sys_port_create("pipe_in", 256);
        stages[i].output_port = (i < stage_count - 1)
            ? stages[i + 1].input_port  // Connect to next stage's input
            : shell_output_port;         // Last stage outputs to shell display
    }

    // 3. Spawn micro-programs for each stage
    for (uint32_t i = 0; i < stage_count; i++) {
        if (is_builtin(stages[i].command)) {
            stages[i].mprog_id = spawn_builtin(stages[i].command,
                                                stages[i].input_port,
                                                stages[i].output_port);
        } else {
            // Lookup command in graph store
            object_id_t cmd_oid = resolve_command(stages[i].command);
            stages[i].mprog_id = sys_spawn_from_oid(cmd_oid,
                                                     stages[i].input_port,
                                                     stages[i].output_port);
        }
    }

    // 4. Feed initial query results into first stage
    // ...
}
```

---

## 5. AI Integration

### 5.1 Context-Aware Autocomplete

As the user types, the shell sends context to `sys_infer` for intelligent completion:

```c
typedef struct {
    char    input_buffer[1024];     // Current input line
    uint32_t cursor_pos;

    // Context: recent commands, current graph position, visible objects
    char    recent_commands[4096];
    char    current_context[2048];  // Labels of nearby vertices
} completion_context_t;

void shell_request_completion(completion_context_t *ctx) {
    // Build prompt for sys_infer
    char prompt[8192];
    snprintf(prompt, sizeof(prompt),
        "You are the Helios OS shell assistant.\n"
        "Current context: %s\n"
        "Recent commands:\n%s\n"
        "The user is typing: %s\n"
        "Cursor is at position %u.\n"
        "Suggest 3 completions:\n",
        ctx->current_context,
        ctx->recent_commands,
        ctx->input_buffer,
        ctx->cursor_pos);

    sys_infer_request_t req = {
        .context_cap = cap_for_buffer(prompt, strlen(prompt)),
        .context_len = strlen(prompt),
        .max_tokens = 128,
        .temperature = 0.3,
        .flags = INFER_FLAG_JSON,   // Structured output
    };

    sys_infer_result_t result = sys_infer(&req);
    // Parse JSON result → display completion candidates
}
```

### 5.2 Natural Language Commands

Prefix a command with `!` to use natural language:

```bash
# Natural language → graph query translation
! find all the kernel source files I modified last week

# sys_infer translates to:
# :source #kernel @authored_by(me) @modified_after(7d_ago)
```

### 5.3 Error Explanation

When a command fails, the shell can explain the error:

```bash
> @child_of(#nonexistent)
Error: No vertices match tag "nonexistent"

# Shell automatically offers:
  AI suggestion: Did you mean #networking? (closest match by embedding similarity)
```

---

## 6. Built-In Commands

### 6.1 System Commands

| Command | Description |
|---------|-------------|
| `ps` | List running micro-programs |
| `kill <id>` | Terminate a micro-program |
| `stat` | System resource usage (CPU, memory, NVMe, NPU) |
| `lspci` | List PCIe devices |
| `lsdev` | List bound drivers and devices |
| `snapshot [label]` | Create a system snapshot |
| `snapshots` | List all snapshots |
| `restore <oid>` | Restore to a snapshot |
| `net` | Network configuration and status |
| `reboot` | Reboot the system |
| `shutdown` | Power off |

### 6.2 Graph Navigation

| Command | Description |
|---------|-------------|
| `cd <query>` | Change context vertex (conceptual "current directory") |
| `ls` | List edges from current context vertex |
| `pwd` | Show current context vertex and its edges |
| `tree [depth]` | Show subtree from current context |
| `info <query>` | Detailed vertex + edge info |
| `history` | Transaction history for a vertex |

### 6.3 Object Manipulation

| Command | Description |
|---------|-------------|
| `new <type> <label>` | Create a new vertex |
| `store <data>` | Store a new object from stdin/input |
| `edit <query>` | Edit an object (creates new version) |
| `rm <query>` | Soft-delete a vertex |
| `cp <src> <dst>` | Copy object (create vertex pointing to same OID) |
| `mv <src> <dst>` | Re-link vertex to different parent |

---

## 7. Shell Rendering

The shell renders its output via the compositor data matrix:

```c
// Shell claims a region in the data matrix
cap_token_t shell_region = sys_ui_claim_region(
    40,     // 40 rows
    120,    // 120 columns
    1.0,    // Full weight (fills available space)
    "shell"
);

// Write a line of output
void shell_write_line(uint32_t row, const char *text,
                      uint32_t fg_color, uint32_t bg_color) {
    matrix_cell_t cell = {
        .type = CELL_TYPE_TEXT,
        .flags = CELL_DIRTY,
        .owner_mprog = shell_mprog_id,
        .text = {
            .fg_color = fg_color,
            .bg_color = bg_color,
            .font_id = FONT_SYSTEM_MONO,
            .font_size = 14 * 64,   // 14pt in 1/64th units
            .style = 0,
        },
    };
    strncpy(cell.text.text, text, 255);
    cell.text.text_len = strlen(text);

    sys_ui_write_cell(&shell_region, row, 0, &cell);
}
```

---

## 8. Configuration

Shell configuration is stored as an object in the graph store:

```c
typedef struct {
    // Prompt
    char        prompt_format[256];     // e.g., "{context.label} > "
    uint32_t    prompt_color;

    // Colors
    uint32_t    fg_color;
    uint32_t    bg_color;
    uint32_t    error_color;
    uint32_t    highlight_color;
    uint32_t    completion_color;

    // Behavior
    bool        ai_autocomplete;        // Enable/disable AI completion
    float       completion_temperature;
    uint32_t    history_size;
    bool        syntax_highlighting;

    // Keybindings (Emacs-style by default)
    struct {
        uint32_t keycode;
        uint32_t modifiers;
        char     action[32];
    } keybindings[64];
} shell_config_t;
```

---

*Next: [12-BUILD.md](./12-BUILD.md) — Build System & Cross-Compilation*
