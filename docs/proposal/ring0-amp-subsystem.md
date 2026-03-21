# Proposal: Ring 0 AMP Configuration & Messaging Subsystem

**Date:** March 20, 2026  
**Status:** DRAFT — Pre-implementation proposal  
**Author:** BadKraft  
**Scope:** `sigma.core` kernel — Ring 0 bootstrap layer  

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [Motivation](#2-motivation)
3. [What AMP Is — and Why It Is the Right Dialect](#3-what-amp-is--and-why-it-is-the-right-dialect)
4. [The Ring 0 AMP Scanner](#4-the-ring-0-amp-scanner)
5. [User-Space Perspective](#5-user-space-perspective)
6. [OS-Level Perspective](#6-os-level-perspective)
7. [The Broader Ring 0 Messaging Subsystem](#7-the-broader-ring-0-messaging-subsystem)
8. [sigma.memory as the First Consumer](#8-sigmamemory-as-the-first-consumer)
9. [Component Roadmap](#9-component-roadmap)
10. [Design Constraints and Non-Goals](#10-design-constraints-and-non-goals)
11. [Open Questions](#11-open-questions)
12. [AMP as Event Pipeline Backbone](#12-amp-as-event-pipeline-backbone)
13. [Security Analysis](#13-security-analysis)
14. [Anvil.Lite — The Zero-Heap AMP Reference Parser](#14-anvillite--the-zero-heap-amp-reference-parser)

**Addenda**

- [Addendum A — SYS0 Reserved Pad Governance](#addendum-a--sys0-reserved-pad-governance)

---

## 1. Executive Summary

Every sigma component that requires configuration today either hardcodes constants or depends on
a bootstrap order that cannot tolerate allocating memory to read its own setup. This proposal
introduces a **Ring 0 AMP scanner** — a zero-allocation, syscall-only subset of the Anvil
Messaging Protocol parser — that runs before any allocator exists, and becomes the foundation
for a **Ring 0 messaging subsystem** shared by all sigma kernel-level components.

The scanner is not a general-purpose Anvil parser. It intentionally implements only the AMP
dialect's most restricted subset: flat scalar statements with no objects, arrays, or attribute
blocks. That constraint maps perfectly onto the operational envelope of Ring 0: read-only,
pre-heap, stack-only, pure C.

`sigma.memory` is the first and most obvious consumer — it needs to know its arena sizes before
the first `mmap()` call — but the design is explicitly not memory-specific. Any component that
reads a `.anvl` config file before its allocator starts, or that emits kernel-level events
(panics, health pulses, state changes) on a channel that predates the heap, benefits from
exactly this subsystem.

---

## 2. Motivation

### 2.1 The Bootstrap Paradox

A memory manager cannot use itself to read its own configuration. The full Anvil parser calls
`Memory.alloc()` internally. The configuration that determines the arena size must exist before
`bootstrap_sys0()` is called. This is not unique to sigma.memory:

| Component | Needs config at | Has allocator at |
|-----------|-----------------|-----------------|
| `sigma.memory` | before `bootstrap_sys0()` | after `bootstrap_sys0()` |
| `sigma.tasking` | before fiber scheduler init | after scheduler init |
| `sigma.net` | before socket buffer pools | after buffer pools |
| `sigma.ipc` | before ring buffer init | after ring buffer init |
| OS-level kernel module | before page-frame init | after page-frame init |

Every one of these components has the same paradox. Without a shared Ring 0 scanner, each one
invents its own bespoke config reader — fragmented, untested, inconsistent format.

### 2.2 The Messaging Gap

At Ring 0, there is no event bus, no log stream, and no structured channel for a component to
say "I initialized at these parameters", "I panicked for this reason", or "my health state is X".
`printf()` is banned. `syslog()` assumes a heap. As a result, Ring 0 failures are currently
either silent or result in raw `abort()` with no context.

A Ring 0 messaging primitive — even a minimal one — lets kernel components emit structured AMP
messages into a fixed static buffer that higher rings (or a host debugger) can read without
the heap being involved on either side.

---

## 3. What AMP Is — and Why It Is the Right Dialect

AMP (Anvil Messaging Protocol) is a dialect of ANVL activated by a `#!amp` shebang. Its defining
property is restriction: only scalars and blob payloads are permitted. Objects, arrays, inheritance,
and attributes are explicitly illegal at the protocol level.

That restriction is not a limitation — it is the entire point. AMP was designed for structured
messaging over arbitrary transports with zero parser state. Those properties align exactly with
Ring 0 requirements:

| AMP Property | Ring 0 Requirement |
|---|---|
| Flat scalars + non-nested scalar arrays/tuples | Config files have no object nesting — trivially stack-parseable |
| Objects, inheritance, attributes forbidden | Eliminates all recursive descent cases — scanner stays linear |
| No allocations inside the parser | Cannot call `malloc()` before heap exists |
| Zero-copy: values are spans into source buffer | Config buffer sits in BSS/stack — no copies |
| Blob payloads are opaque to the parser | Kernel event payloads are opaque to consumers |
| Parser holds no internal state after return | Ring 0 scanner is stack-frame-local; no globals |
| Single entry point `anvl_parse(context)` | Scanner is a single function call |

The full AMP parser (`anvil.o`) is too large to embed in Ring 0 — it carries the entire Anvil
runtime. But nothing in the Ring 0 path requires resolvers, writers, attribute blocks, or AST
traversal. The **Ring 0 AMP scanner** implements the 5% of AMP that matters here, and does so
using only the ISO C standard library intrinsics that are available even in freestanding
environments (`<stddef.h>`, `<stdint.h>`, no heap).

### 3.1 The Upgrade Path

Config files written for the Ring 0 scanner are valid AMP files. When `sigma.anvil.o` is
packaged and available, higher-ring consumers can parse the same `.anvl` files with the full
parser and get richer features (inheritance, `$var` resolution, schema validation) without
touching the file format. The Ring 0 scanner is a strict subset, not a fork.

---

## 4. The Ring 0 AMP Scanner

### 4.1 Operational Envelope

```
┌──────────────────────────────────────────────────────┐
│  RING 0 AMP SCANNER                                  │
│                                                      │
│  Input:   const char *buf  (stack or BSS, ≤ 512 B)  │
│           usize            len                       │
│  Output:  struct r0_amp_msg_s[] (stack-local array)  │
│                                                      │
│  No heap. No globals. No syscalls except open/read/  │
│  close for file I/O. No Anvil runtime dependency.    │
│                                                      │
│  Handles (valid AMP subset):                         │
│    key := integer           ← config value           │
│    key := "string"          ← config string (span)  │
│    key := [1, 2, 3]         ← scalar array (flat)   │
│    key := (true, 0, "ok")   ← scalar tuple (flat)   │
│    // comment               ← ignored                │
│    #!amp                    ← shebang, ignored       │
│    key := integer KB|MB|GB  ← scale suffixes        │
│                                                      │
│  Silently skips (non-AMP constructs):                │
│    Objects  { … }                                    │
│    Attributes @[ … ]                                 │
│    $var references                                   │
│                                                      │
│  Hard errors (AMP violations):                       │
│    Nested arrays / tuples   ← AmpArrayElementNotScalar│
│    Inheritance  : Base      ← AmpInheritanceNotAllowed│
└──────────────────────────────────────────────────────┘
```

### 4.2 Core Types

```c
// A single parsed key := value statement
typedef struct r0_amp_kv_s {
    const char *key;      // pointer into source buffer (zero-copy)
    uint16_t    key_len;
    int64_t     i64;      // integer value (0 if not integer type)
    const char *str;      // pointer into source buffer for string values
    uint16_t    str_len;
    uint8_t     type;     // R0_AMP_INT | R0_AMP_STR
} r0_amp_kv_s;

// Scanner result — stack-allocated, caller provides capacity
typedef struct r0_amp_doc_s {
    r0_amp_kv_s *entries;
    uint16_t     capacity;
    uint16_t     count;
} r0_amp_doc_s;
```

### 4.3 The Four Scanner Functions

```c
// Parse a buffer already in memory (BSS config, embedded defaults)
bool r0_amp_scan(const char *buf, usize len, r0_amp_doc_s *out);

// Open a file, read ≤ 512 bytes, scan it, close file descriptor
bool r0_amp_scan_file(const char *path, r0_amp_doc_s *out);

// Look up a key in a parsed document
const r0_amp_kv_s *r0_amp_get(const r0_amp_doc_s *doc, const char *key, uint16_t key_len);

// Convenience: get integer with fallback default
int64_t r0_amp_get_i64(const r0_amp_doc_s *doc, const char *key, int64_t default_val);
```

### 4.4 Size Budget

The scanner implementation targets ≤ 200 lines of C. No dependencies beyond `<stdint.h>`,
`<stdbool.h>`, `<fcntl.h>`, and `<unistd.h>`. When compiled into `sigma.core.module.o`, it
adds negligible code size.

---

## 5. User-Space Perspective

For a user-space application using sigma components, the Ring 0 AMP scanner is invisible —
it runs before `main()` returns the first line of user code. Its effect is that components
initialize with the parameters the application requested.

### 5.1 Config File Discovery

The scanner follows a fixed search order. Applications get the level of control they need
without any runtime configuration API:

```
1. $SC_<COMPONENT>_CONFIG   (explicit env var — max priority)
2. ./<component>.anvl        (application-local, next to binary)
3. ~/.config/sigma/<component>.anvl  (user-level)
4. /etc/sigma/<component>.anvl       (system-wide)
5. Compiled-in defaults              (no file needed)
```

For `sigma.memory`, the component name is `memory`, so the local file is `./memory.anvl`.

### 5.2 What a User-Space Config File Looks Like

```
#!amp
// memory.anvl — sigma.memory configuration for MyApp

arena_size := 4MB    // kernel arena (default 2MB; valid: 2MB, 4MB, 8MB)
slab_size  := 128KB  // SLB0 size (default 64KB)
```

The size-suffix support (`KB`, `MB`, `GB` → multiply factor) is handled in `r0_amp_scan` itself.
The file is ~ 3 lines. The format is self-documenting. No ini parser, TOML library, or JSON
schema required.

### 5.3 Zero Cost Without a File

If no `memory.anvl` exists, the scanner path short-circuits at the filesystem check and
`init_memory_system()` proceeds with compiled-in defaults. The scanner adds no overhead to the
common case of applications that accept defaults.

### 5.4 Visibility: Querying the Active Config

After bootstrap, `sys0_config_ptr()` returns the active configuration stored in the SYS0
Reserved pad. Applications (or diagnostic tools) can query it without triggering any allocation:

```c
sc_memory_config_s *cfg = Allocator.config();   // proposed facade accessor
// cfg->arena_size → whatever was resolved at init time
// cfg->slab_size  → same
```

---

## 6. OS-Level Perspective

At the OS level, the Ring 0 scanner addresses a gap in how kernel-level sigma components
are configured and how they communicate before any managed memory exists.

### 6.1 Configuration Without a Filesystem Dependency Loop

A kernel memory module cannot read its config from a filesystem that itself requires the memory
module to be initialized. The Ring 0 scanner breaks this loop via the search-order fallback:

- In a fully bootstrapped OS, `/etc/sigma/memory.anvl` is authoritative.
- In an early boot environment before `/etc` is mounted, `$SC_MEMORY_CONFIG` points to a
  synthesized config in a ramdisk or UEFI variable.
- In a stripped minimal build, the compiled-in defaults are the only source — no file I/O at all.

All three paths use the same scanner, the same config struct, and the same validation logic.

### 6.2 SYS0 as a Cross-Ring Configuration Channel

SYS0 is a fixed 8 KB mmap at Ring 0. The Reserved pad at offset 64 (128 bytes) stores the
active `sc_memory_config_s` after bootstrap. Higher rings (Ring 1 services, user-space daemons)
that need to know "how much kernel arena do we have?" can read from the SYS0 base address
directly — no syscall, no allocation, no IPC round-trip. This is the same pattern used by the
Linux `vDSO` and Windows `KUSER_SHARED_DATA`: a read-only shared page that exposes kernel state
to user space without a context switch.

### 6.3 Panic and Health Reporting Without the Heap

The Ring 0 messaging subsystem (Section 7) enables kernel components to emit structured AMP
messages into a static ring buffer in SYS0 DAT before the heap exists. A firmware debugger or
host monitor can parse these using the same AMP scanner — because the messages are valid AMP.

---

## 7. The Broader Ring 0 Messaging Subsystem

sigma.memory is the first consumer, but the Ring 0 AMP scanner should be designed from day one
as a shared subsystem. The following shows the value it delivers beyond configuration.

### 7.1 Kernel Event Emission

```
RING 0 EVENT BUFFER (in SYS0 DAT, fixed size, circular)

Slot N:
  #!amp
  event  := "bootstrap.complete"
  module := "sigma.memory"
  arena  := 2097152
  slab   := 65536
  seq    := 7
```

Events are AMP messages. The scanner that reads config files is the same one that reads event
slots. There is no second format to learn, no second parser to maintain.

### 7.2 Cross-Component Handshake

Before a managed IPC channel exists, two kernel-level components can rendezvous through AMP
messages in shared SYS0 space:

```
# sigma.memory writes into SYS0 event slot:
event  := "ready"
module := "sigma.memory"
r7     := 140234567890432   // address of SLB0 controller

# sigma.tasking reads it and resolves its allocator:
r7_addr := r0_amp_get_i64(&doc, "r7", 0);
// cast to sc_ctrl_base_s* and proceed
```

No dynamic linking assumptions. No global symbol resolution. No heap.

### 7.3 Module Health Protocol

Ring 0 components can maintain a static AMP health record in SYS0 DAT, updated in-place as
state transitions occur. A watchdog (hardware or software) reads the slot periodically. If the
last update timestamp is stale, the watchdog acts.

```
#!amp
module := "sigma.memory"
state  := "ready"           // ready | degraded | shutdown
arena_used  := 1048576
arena_total := 2097152
tick   := 183742
```

This is zero-overhead monitoring. The record is valid AMP. Any tool with the Ring 0 scanner —
including a remote debugger over a JTAG link — can decode it.

### 7.4 Subsystem Summary

| Capability | Ring 0 AMP provides | Alternative without it |
|---|---|---|
| Pre-heap component config | `r0_amp_scan_file()` | Bespoke per-component reader, inconsistent format |
| Post-bootstrap config visibility | SYS0 Reserved pad | Opaque runtime constant |
| Panic / fault logging | AMP event slot in SYS0 DAT | Silent `abort()` or raw `printf()` |
| Cross-component rendezvous | AMP message in shared slot | Raw address passing, brittle |
| Health monitoring | Static AMP health record | No structured state at Ring 0 |
| Upgrade path to full Anvil | Config files are valid AMP | Migration + format divergence |

---

## 8. sigma.memory as the First Consumer

The implementation plan for sigma.memory's use of the Ring 0 scanner is:

### 8.1 Config File (`memory.anvl`)

```
#!amp
// sigma.memory runtime configuration
// Place next to your binary (./memory.anvl) or in /etc/sigma/memory.anvl

arena_size := 2MB   // kernel arena (reclaim_allocator backing);  valid: 2MB | 4MB | 8MB
slab_size  := 64KB  // SLB0 size (reclaim_allocator bootstrap);   valid: 64KB | 128KB | 256KB
```

### 8.2 Bootstrap Integration

```
init_memory_system()
  │
  ├─ memory_config_load()          ← r0_amp_scan_file() search order
  │    └─ returns sc_memory_config_s cfg (stack value, no allocation)
  │
  ├─ bootstrap_sys0()
  │    └─ writes cfg into SYS0 Reserved pad after mmap
  │
  ├─ bootstrap_kernel()
  │    └─ mmap(KNL_SLAB_SIZE) → reads arena_size from sys0_config_ptr()
  │
  └─ bootstrap_slb0()
       └─ mmap(SLB0_SIZE)     → reads slab_size from sys0_config_ptr()
```

### 8.3 module.c Integration

```c
static int memory_module_init(void *ctx) {
    (void)ctx;
    sc_memory_config_s cfg = memory_config_load();   // Ring 0 AMP scan
    init_memory_system(cfg);
    return OK;
}
```

---

## 9. Component Roadmap

| Phase | Component | Deliverable |
|---|---|---|
| **P0** | `sigma.core` | Add `r0_amp_scan`, `r0_amp_scan_file`, `r0_amp_get` to `sigma.core.module.o` |
| **P1** | `sigma.memory` | `memory_config_load()` using Ring 0 scanner; CFG test suite |
| **P2** | `sigma.core` | SYS0 DAT event buffer layout; `r0_amp_emit()` for kernel event slots |
| **P3** | `sigma.tasking` | Config reader for fiber pool sizes; Ring 0 rendezvous with sigma.memory |
| **P4** | `sigma.net` | Socket buffer pool config; Ring 0 health record |
| **P5** | `sigma.ipc` | IPC ring buffer size config; cross-component handshake protocol |
| **P6** | All | Health monitoring spec; watchdog integration |

P0 and P1 are the immediate deliverables. P2–P6 are the broader Ring 0 messaging subsystem
build-out; they can proceed independently as each component is developed.

---

## 10. Design Constraints and Non-Goals

### Constraints (never broken)

- **No heap at Ring 0** — scanner uses stack buffers only; no `malloc()`, no `Allocator.alloc()`;
  streaming line-by-line reads with an ~80-byte per-line stack buffer eliminate any fixed file-size cap
- **No Anvil runtime dependency** — the scanner does not link against `anvil.o`
- **Config files are valid AMP** — shebang `#!amp` is enforced; missing or incorrect shebang
  causes the scanner to reject the file, emit a Ring 0 event, and fall back to compiled-in defaults;
  the full Anvil parser treats `#!amp` as a strict-mode activator, so files are parse-compatible
- **Freestanding-compatible** — the scanner must compile in environments without `libc` (`#include
  <unistd.h>` is gated; an abstraction layer handles file I/O on metal or hosted)
- **Single-reader, single-writer for SYS0 event slots** — no locking; atomic writes are
  the responsibility of the emitter; higher rings are read-only observers

### Non-Goals

- **Not a general-purpose Anvil parser** — does not handle objects, nested scalars (arrays of
  arrays, tuples of tuples), inheritance, `$var` resolution, or attribute blocks; flat scalar
  arrays (`key := [1, 2, 3]`) and flat scalar tuples (`key := (1, "foo")`) are valid AMP and
  are handled by the scanner; elements are stored as flat value spans, not recursively parsed
- **Not a runtime reconfiguration mechanism** — config is read once at init time; there is no
  live reload, no watch, no signal handler
- **Not a replacement for `sigma.anvil.o`** — the Ring 0 scanner is for pre-heap bootstrap only;
  post-heap config and scripting use the full Anvil runtime
- **Not an IPC transport** — AMP messages in SYS0 are a low-level diagnostic/rendezvous channel,
  not a production message bus; `sigma.ipc` owns that space

---

## 11. Open Questions

1. ~~**Size-suffix scope**~~ — **Resolved:** `KB`/`MB`/`GB` suffixes are in v1. Non-negotiable
   ergonomics — authoring `arena_size := 2MB` vs `arena_size := 2097152` is the difference between
   a config file that reads like documentation and one that reads like a hex dump.

2. ~~**SYS0 event buffer geometry**~~ — **Resolved:** 8 slots × 128 bytes = 1 KB in SYS0 DAT.
   Geometry committed in P2 scope once DAT utilization is audited. Offset constant reserved now;
   see Addendum A.

3. ~~**Ownership of the Ring 0 scanner**~~ — **Resolved:** `r0_amp_*` lives in `sigma.core.module.o`
   for P0/P1. The scanner is ~200 lines with no external dependencies. Relocation to a dedicated
   `sigma.amp.o` is a clean one-step refactor once Sigma.Build/Sigma.Pack are operational.

4. **Freestanding file I/O abstraction** — `r0_amp_scan_file()` calls `open()`/`read()`/`close()`.
   On metal (no OS), file I/O doesn't exist; config must come from a BSS block or firmware variable.
   Decision: bake in the POSIX path for P0. The `r0_amp_file_reader_fn` function pointer hook
   (~5 lines) will be added when a metal target materializes — it does not block P0.

5. **Config validation** — **Resolved:** enforce valid ranges with hard fallback to defaults.
   Invalid values clamp to nearest valid (floor for too-small, cap for too-large), set the
   appropriate `SC_R0_MFLAG_*_CLAMPED` flag in `sc_memory_config_s.flags`, and emit a Ring 0
   event. Minimum: `arena_size ≥ 2MB` (power-of-2 multiple), `slab_size ≥ 64KB` (power-of-2).
   Silently accepting `arena_size := 7` is a worse failure mode than ignoring the file entirely.

---

## 12. AMP as Event Pipeline Backbone

The Ring 0 scanner's role does not end at configuration. AMP's structural properties make it
the natural backbone for the entire kernel event pipeline — not just pre-heap config reads, but
IRQ annotations, health pulses, fault records, and cross-component state broadcasts.

### 12.1 Why AMP's Type System Maps Onto Kernel Events

The AMP type vocabulary — integers, strings, scalar arrays, scalar tuples, and opaque blobs —
maps directly onto the data shapes that kernel events actually carry:

| AMP type | Kernel event use case | Example |
|---|---|---|
| Integer | Counter, address, status code, timestamp | `seq := 7`, `arena_used := 1048576` |
| Size-suffixed integer | Capacity, threshold | `arena_total := 2MB` |
| String span | Module name, event class, fault description | `event := "page.fault"` |
| **Scalar array** | IRQ affinity mask, CPU set, multi-value register dump | `cpus := [0, 2, 4, 6]` |
| **Scalar tuple** | Named pair: (address, size), (pid, priority) | `range := (0x7f000000, 4096)` |
| **Blob** | Opaque payload: stack trace, register snapshot, binary fault context | `trace := <128:...>` |

Scalar arrays and tuples are not convenience features — they are what makes AMP viable as an
eventing format rather than a config-only format. A fault record that needs to emit a register
set, or an IRQ handler that needs to annotate a CPU affinity mask, can do so in a single AMP
message without inventing a second encoding.

The blob type seals the deal for Ring 0 events: a stack trace, a register snapshot, or a
binary crash context cannot be expressed as flat scalars. AMP blobs carry opaque binary payloads
with a fixed length prefix — the scanner skips them, the full Anvil parser surfaces them to user
space, and a Ring 0 fault recorder can write them directly from CPU register state with no
encoding conversion.

### 12.2 IRQ Annotation at Ring 0

Without a structured event format, IRQ handling at Ring 0 is entirely opaque — a handler fires,
something happens, the handler returns. There is no record of:
- Which interrupt arrived
- What the CPU state was when it arrived
- What action was taken
- Whether the action succeeded

With AMP event slots in SYS0 DAT, an IRQ handler can write a structured record in ~10 store
operations — no allocation, no heap, no syscall:

```
#!amp
event  := "irq.received"
irq    := 14
module := "sigma.memory"
regs   := (rax, rbx, rcx, rdx)            // scalar tuple — register snapshot
pages  := [0x7f000, 0x7f001, 0x7f002]    // scalar array — affected page indices
trace  := <64:deadbeef...>                // blob — raw stack trace bytes
seq    := 183744
```

The same scanner that reads `memory.anvl` can decode this record. No second parser. No second
format. The IRQ event is valid AMP — a host debugger, a watchdog process, or the full Anvil
runtime can all consume it without any Ring 0-specific tooling.

### 12.3 Pipeline Architecture

```
  ┌────────────────────────────────────────────────────────────────┐
  │  RING 0 EVENT PIPELINE                                         │
  │                                                                │
  │  Emitters (pre-heap):                                          │
  │    sigma.memory bootstrap → AMP slot in SYS0 DAT              │
  │    IRQ handlers            → AMP slot in SYS0 DAT             │
  │    sigma.tasking scheduler → AMP slot in SYS0 DAT             │
  │    Fault / panic handlers  → AMP slot in SYS0 DAT (+ blob)    │
  │                                                                │
  │  Transport:                                                    │
  │    Fixed circular ring in SYS0 DAT (8 slots × 128 B = 1 KB)   │
  │    Single-writer per slot; higher rings are read-only          │
  │    r0_amp_emit() writes one slot; advances ring head           │
  │                                                                │
  │  Consumers (any ring, no allocation required):                 │
  │    Ring 1 services    → r0_amp_scan() on next slot             │
  │    User-space daemons → read SYS0 base (read-only mmap)        │
  │    Host debugger      → JTAG / gdbserver reads SYS0 address    │
  │    Full Anvil runtime → sigma.anvil.o parses slots post-heap   │
  └────────────────────────────────────────────────────────────────┘
```

The pipeline requires zero additional infrastructure. The emitter is a single `r0_amp_emit()`
call. The transport is a fixed-size ring already budgeted in SYS0 DAT. The consumers use the
same `r0_amp_scan()` already implemented for config reads. The format is the same AMP that
config files use — same scanner, same tooling, same upgrade path to the full Anvil runtime.

### 12.4 Scalar Arrays and Tuples Close the Gap

Without scalar arrays and tuples, a CPU affinity mask requires either a bespoke encoding
(`cpu0 := 1`, `cpu1 := 0`, `cpu2 := 1` ...) or a binary blob that the scanner cannot
inspect. With scalar arrays:

```
affinity := [0, 2, 4, 6]   // CPUs handling this interrupt
```

This is directly human-readable, directly machine-parseable by the Ring 0 scanner, and
directly consumable by the full Anvil runtime. The same is true for register pairs, address
ranges, and any multi-valued kernel primitive. Scalar arrays and tuples are the feature that
makes AMP events composable rather than merely informational.

---

## 13. Security Analysis

The security posture of the Ring 0 AMP subsystem is a product of its structural constraints.
No policy is being enforced by convention — the constraints are architectural. A security
specialist reviewing this design would identify the following attack surface reduction
characteristics.

### 13.1 Stream Parsing Eliminates Buffer-Oriented Attack Vectors

The scanner reads forward through an ~80-byte per-line stack buffer, discards the buffer
on line completion, and holds no cumulative parse state between lines. This architectural
choice eliminates entire classes of vulnerability:

| Attack class | Eliminated because |
|---|---|
| **Buffer overflow** | Input line is bounded to stack buffer; scanner never accumulates across lines |
| **Heap spray / UAF** | No heap allocation at any point in the scan path |
| **Format string injection** | Scanner emits no formatted output; all diagnostic emission is bounded AMP |
| **TOCTOU (config file)** | File is opened, read line by line, and closed in a single locked call; no re-read |
| **Integer overflow in sizes** | Size suffixes multiply against `int64_t` with explicit range check before storage |
| **Null termination attacks** | Scanner uses explicit `key_len`/`str_len` spans; no `strlen()` in the parse path |

The scan path from `open()` to `close()` has no branch in which a heap pointer is
touched. An attacker who can write an arbitrary `memory.anvl` can at most supply out-of-range
values, which are clamped and flagged — the system initializes with safe defaults and records
the anomaly in a Ring 0 event.

### 13.2 AMP's Structural Restriction Is a Security Primitive

The `#!amp` shebang is not only a strict-mode signal — it is an integrity gate. The Ring 0
scanner rejects any file that does not open with `#!amp`. This means:

- A config file that was overwritten with a shell script, an ELF binary, or arbitrary bytes
  fails the shebang check immediately and is discarded
- A config file that was injected via a symlink pointing to `/etc/passwd` or `/proc/cmdline`
  fails the shebang check immediately
- A partial write that truncated the shebang fails the check and falls through to defaults

The shebang check is the first operation in `r0_amp_scan()`. It costs 6 bytes of comparison.
It provides integrity verification with no cryptographic overhead.

### 13.3 SYS0 as a Hardened Read-Only Channel for Higher Rings

After `bootstrap_sys0()` completes, the config region in the SYS0 Reserved pad is written
once and never modified. Higher rings see it via a read-only memory mapping (the vDSO-pattern
described in Section 6.2). The security properties:

- **No write path for user space** — user-space consumers receive a read-only mmap of the SYS0
  base; there is no syscall, no ioctl, and no writable handle that could allow a user-space
  process to overwrite the active config at runtime
- **No deserialization at read time** — `sys0_r0_config()` is a cast of the SYS0 base address;
  there is no parsing, no allocation, no format conversion involved in reading the active config;
  the attack surface of a consumer that queries `arena_size` is a single pointer dereference
- **Audit trail via loaded bitmask** — `sc_sys0_config_region_s.loaded` records whether each
  slot was sourced from a file or from compiled-in defaults; `flags` in each slot records
  clamping events; a post-mortem analysis of a crash can determine exactly what config was
  active and whether it came from an unexpected source

### 13.4 Event Slots: Bounded, Append-Only, Observer-Transparent

The Ring 0 event ring in SYS0 DAT is a bounded circular buffer. Each slot is fixed-size
(128 bytes). The write protocol is single-writer-per-slot with no locks:

- **Bounded size** — the ring cannot grow; an emitter cannot exhaust memory by flooding events
- **No pointer following** — event slots hold flat AMP text; there are no pointers to dereference,
  no linked lists to walk, no vtables to overwrite
- **Observer transparency** — any process with the SYS0 base address can read any slot without
  any capability grant; there is no secret channel, no opaque encoding, no proprietary wire
  format; a human with a hex editor can read the events
- **Blob payloads are opaque to the scanner** — a blob in an event slot (e.g., a register
  snapshot) is length-prefixed and skip-scanned; the scanner never dereferences blob content;
  a malformed blob length causes the scanner to discard the slot and continue, not to crash

### 13.5 What the Subsystem Does Not Protect Against

Honest security analysis requires stating the limits:

- **Config file integrity** — the scanner has no cryptographic verification of config files;
  a privileged attacker who can write `memory.anvl` before sigma.memory initializes can
  supply valid-but-adversarial config values (e.g., `arena_size := 2MB` with `slab_size := 0`
  to trigger a degenerate state). Mitigation: config validation with mandatory defaults fallback
  (Section 11, Q5) prevents zero/negative values; value-range clamping is the defense.
- **Physical memory access** — an attacker with DMA access or physical memory access can write
  directly to the SYS0 base address after bootstrap and overwrite the config region. This is
  below the operating model of the subsystem; OS-level memory protection (SMEP, SMAP, IOMMU)
  is the applicable defense layer.
- **SYS0 address leakage** — the SYS0 base address is an `addr` in private process state; ASLR
  applies to the mmap. If the address leaks via a side channel, a user-space attacker with a
  writeable mapping could tamper with config. The read-only mmap for user-space consumers is
  the primary mitigation; a writable diagnostic path should never be exposed.

---

## 14. Anvil.Lite — The Zero-Heap AMP Reference Parser

### 14.1 Rationale

The Ring 0 AMP scanner described in Section 4 has requirements no existing Anvil parser
satisfies: it must run before any allocator initializes, cannot touch the heap, and processes
a strictly constrained dialect with no objects, no nested structures, and no variable
references. These constraints are features, not limitations — and they justify a distinct,
named artifact: **Anvil.Lite**.

Anvil.Lite is not a stripped-down version of `sigma.anvil.o`. It is purpose-built around
AMP's grammar, with structural restrictions encoded directly into the parser state machine
rather than enforced as runtime validation checks.

### 14.2 Parser Comparison

| Parser | Language | Heap | Full ANVL | AMP | Freestanding | Target |
|---|---|---|---|---|---|---|
| `sigma.anvil.o` | C | Yes | ✅ | ✅ | No | sigma.* runtime |
| `Anvil.Net` | C# | Yes | ✅ | ✅ | No | .NET / toolchain |
| **Anvil.Lite** | C | **No** | ❌ | ✅ | **Yes** | Ring 0 / embedded |
| sparky | TBD | — | TBD | — | — | scripting / automation |

The **zero-heap** and **freestanding** columns are the defining properties. Any parser
requiring `malloc` or platform-level dynamic linking is not a Ring 0 candidate.

### 14.3 Design Constraints

- **Zero heap.** All working state lives on the caller's stack. No `malloc`, `calloc`, or
  arena calls. The line buffer is a fixed-size stack-local array (~80 bytes).
- **Line-by-line streaming.** The parser never buffers the entire file. One line is read,
  dispatched, and discarded. This eliminates both the 512-byte file-size floor and the
  string-span lifetime hazard present in any buffer-then-parse design.
- **`#!amp` enforcement.** The first six bytes must match `#!amp\n` exactly. Any file that
  does not open with this header is rejected before the first key-value pair is scanned.
  This is a hard 6-byte check — not a heuristic — ensuring Anvil.Lite is never accidentally
  invoked on a plain ANVL file.
- **AMP type set only.** Recognised: bare scalars, quoted strings, size-suffixed integers
  (`K`, `M`, `G`, `T`, `P`), flat scalar arrays (`[ v0, v1, v2 ]`), flat scalar tuples
  (`( v0, v1, v2 )`), binary blobs. Any object brace, nested structure, inheritance marker,
  or `$var` reference is a parse error, not a warning.
- **~200–500 lines of C.** Small enough to be manually audited. External dependencies are
  limited to `<stdint.h>` and `<string.h>`.

### 14.4 Reference Implementation Lifecycle

**P0/P1:** Anvil.Lite lives inside `sigma.core.module.o` as the internal AMP scanner powering
Ring 0 bootstrap. It is not a separately linkable artifact at this stage; it is an internal
implementation detail of the `sigma.core` module.

**Post-P1:** Once the scanner has stabilised through at least one full Ring 0 bootstrap cycle,
it is extracted and promoted to a standalone object: **`anvil.lite.o`**.

At that point `anvil.lite.o` becomes independently linkable for:
- Bare-metal and RTOS targets that cannot accept `sigma.anvil.o`'s runtime dependencies
- Minimal conformance testing: `main()` reads from stdin; sigma.memory and sigma.test are
  not required in the test harness
- Third-party AMP implementations validated against its reference output

### 14.5 Upgrade Path

Anvil.Lite is explicitly upgrade-compatible with the rest of the parser ecosystem:

- `Anvil.Lite` → `sigma.anvil.o` — same AMP dialect; adds full ANVL object model and runtime
  allocation; zero changes to existing `.anvl` config files required
- `Anvil.Lite` → `Anvil.Net` — same AMP dialect; C# / .NET toolchain implementation

An application that boots via Anvil.Lite config parsing can migrate to `sigma.anvil.o` with
no changes to its `.anvl` files. Only the parser's allocation model changes.

### 14.6 Anvil.Lite as the AMP Standard Reference

Naming Anvil.Lite as a distinct artifact positions it as the specification-grade AMP
conformance reference. The AMP dialect specification is Section 3 of this proposal.
Anvil.Lite is the normative implementation of that specification.

A third party implementing AMP parsing can:

1. Read Section 3 for the grammar and type constraints
2. Run Anvil.Lite against the conformance corpus in `sigma.core/test/amp/` (to be established
   in P1 scope)
3. Certify parser output against the reference

This gives AMP a portable licensing surface independent of the sigma framework, without
fragmenting the dialect definition.

---

## Addendum A — SYS0 Reserved Pad Governance

**Status:** Ratified March 20, 2026  
**Implementation:** Pending — design ratified; implementation deferred until Ring 0 bootstrap phase begins

### A.1 Committed Layout

The SYS0 Reserved pad (offset 64, 128 bytes) is committed as the Ring 0 config region,
governed by `sc_sys0_config_region_s` defined in `sigma.core/ring0.h`.

```
SYS0 Reserved pad (offset 64 – 191, 128 bytes)
────────────────────────────────────────────────────────────
[  0 –   7]  header: magic (uint32) + loaded bitmask (uint32)  8 bytes
[  8 –  35]  slot[SC_R0_CFG_MEMORY]   — sc_memory_config_s    28 bytes
[ 36 –  63]  slot[SC_R0_CFG_TASKING]  — future                28 bytes
[ 64 –  91]  slot[SC_R0_CFG_NET]      — future                28 bytes
[ 92 – 119]  slot[SC_R0_CFG_IPC]      — future                28 bytes
[120 – 127]  _reserved — version nibble, diagnostic flags      8 bytes
────────────────────────────────────────────────────────────
Total: 128 bytes — exactly fills the Reserved pad.
```

A compile-time `_Static_assert` in `internal/memory.h` enforces that `sizeof(sc_sys0_config_region_s) == 128`.

### A.2 Slot Ownership Rules

- Each slot index is owned by exactly one sigma component.
- sigma.core defines slot indices (`sc_r0_config_slot_e`) and byte sizes only. It has no
  knowledge of any slot's internal layout.
- Each module defines and documents its own config struct. The struct must satisfy
  `sizeof(struct) ≤ SC_R0_CONFIG_SLOT_SIZE` (enforced by a per-module `_Static_assert`).
- A slot is written exactly once, by its owning module, during Ring 0 bootstrap.
  It is read-only thereafter.

### A.3 Accessor

```c
// sigma.memory/src/memory.c (after bootstrap_sys0 maps SYS0):
static inline sc_sys0_config_region_s *sys0_r0_config(void) {
    return (sc_sys0_config_region_s *)(s_sys0_base + SYS0_R0CONFIG_OFFSET);
}

// Usage (reading the memory config slot after bootstrap):
sc_memory_config_s *cfg =
    (sc_memory_config_s *)sys0_r0_config()->slot[SC_R0_CFG_MEMORY];
```

### A.4 SYS0 DAT Is Not Affected

SYS0 DAT (offset 456, 7736 bytes) is not used for config storage. Its current tenants
(SLB0 reclaim ctrl, SLB0 slab descriptor, KNL ctrl, KNL slab descriptor — approx. 360 bytes)
are fixed at their current offsets. The P2 event ring buffer will claim a named
block at `DAT + SC_SYS0_EVENT_OFFSET` (to be defined in P2 scope). No further geometry
changes are expected at P0/P1.

### A.5 Future Slot Assignment

| Slot | Owner | Config struct | Status |
|---|---|---|---|
| `SC_R0_CFG_MEMORY`  | sigma.memory  | `sc_memory_config_s`  | Defined |
| `SC_R0_CFG_TASKING` | sigma.tasking | `sc_tasking_config_s` | Reserved — P3 |
| `SC_R0_CFG_NET`     | sigma.net     | `sc_net_config_s`     | Reserved — P4 |
| `SC_R0_CFG_IPC`     | sigma.ipc     | `sc_ipc_config_s`     | Reserved — P5 |

---

*This proposal is the design input for `sigma.core` Phase 0 and `sigma.memory` Phase 7.*  
*Next step: team review → ratify or amend Open Questions → begin implementation.*
