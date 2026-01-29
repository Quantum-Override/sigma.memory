# SigmaCore Memory Management System - Design Document v1.0

**Author:** SigmaCore Development Team  
**Date:** January 20, 2026  
**Status:** Design Complete, Ready for Implementation  
**Version:** 1.0

---

## 1. Executive Summary

The SigmaCore memory management system implements a **dual-allocator architecture** designed for deterministic, real-time-safe operation:

- **SYS0**: Bootstrap allocator (4K page) for system objects with reclaiming allocation strategy
- **SLB0**: Default user-space allocator using bump allocation (**Policy**: `DYNAMIC`; **Flags**: `SECURE`)
- **SLBn**: Additional user-space arena allocators using bump allocation; set policy and flags on creation

This design enables:
- ✅ Deterministic allocation/deallocation (no fragmentation in user space)
- ✅ Bulk deallocation via frame nesting (begin/end checkpoints)
- ✅ Policy-driven allocation (fixed arenas vs. dynamic growth)
- ✅ Cross-scope isolation with explicit move/copy semantics
- ✅ Real-time safety through compile-time and runtime invariants

---

## 2. Architecture Overview

### 2.1 Dual Allocator Model

```
┌─────────────────────────────────────────┐
│         User Application                │
└────────────┬────────────────────────────┘
             │ Allocator.alloc() on current scope
             │ Allocator.Scope.alloc(scope) for explicit scope
             ↓
┌─────────────────────────────────────────┐
│     Scopes (General Interface)          │  ← Polymorphic contexts
│                                         │     - Scopes: SYS0, SLB0, Custom Arenas
│  [Allocator.Scope operations]           │     - Frames: Nested checkpoints within Arena
└────────────┬────────────────────────────┘
             │ (polymorphic dispatch via function pointers)
             ↓
┌────────────────────────────────────────────────────────────┐
│              Scope Implementations                         │
│                                                            │
│  SYS0 Scope (Reclaiming)          Arena Scopes (Bump)      │
│  ├─ alloc_fn = sys0_alloc         ├─ Arena.create()        │
│  ├─ dispose_fn = sys0_dispose     ├─ Arena.destroy()       │
│  └─ [Registers] [Metadata]        ├─ Arena.reset()         │
│                                   ├─ Arena.begin_frame()   │
│                      SLB0 Arena    ├─ Arena.end_frame()    │
│                      (DYNAMIC)     └─ Frame (nested scope) │
│                                      - Checkpoint for      │
│                                        bulk deallocation   │
└────────────────────────────────────────────────────────────┘
```

### 2.2 Scope Hierarchy

**Scope** = General allocator interface (polymorphic)
- Abstract type; parameter for generic operations
- Operations: `Allocator.Scope.current()`, `.set()`, `.config()`, `.alloc()`, `.dispose()`
- Concrete implementations: SYS0, Arena, Frame

**Arena** = Concrete scope type with explicit lifecycle
- Created via `Arena.create(policy, flags)` — policy immutable, flags mutable
- Manages page chains and bump allocation
- Supports nested frames via `Arena.begin_frame()` / `Arena.end_frame()`
- Example: SLB0 is an Arena instance with POLICY_DYNAMIC

**Frame** = Temporary scope nested within an Arena
- Created via `Arena.begin_frame()` — returns frame handle
- Checkpoint for allocation state (bump pointer saved)
- On `Arena.end_frame(frame)`, bump restored → all frame-scoped allocations freed
- Frames do NOT exist independently; always owned by an arena

### 2.3 Allocation Strategies

**SYS0 Strategy (Reclaiming):**
- Metadata: block header + footer with next pointer and size
- Suitable for: System objects with known lifetimes (arenas, frames, page chains)
- Fragmentation: Possible; not suitable for app data

**Arena Strategy (Bump per page, chained):**
- Single bump pointer per page; page chains for overflow
- Reset via frames (checkpoint → alloc → restore) or arena reset
- Suitable for: Bulk data with clear lifecycle
- Fragmentation: Zero (except at arena boundary)

**SLB0** = Default arena allocator
- Arena instance with POLICY_DYNAMIC (auto-grows by chaining pages)
- SCOPE_FLAG_SECURE by default (can be modified)
- Provides both `Arena.*` and `Allocator.*` APIs

---

## 3. SYS0 Bootstrap Allocator (4K)

### 3.1 Page Layout

```
Offset      Size    Content                    Purpose
──────────────────────────────────────────────────────────
0           8       R0 Register (addr)         SYS0 base address (offset resolution)
8           48      R1-R6 Registers (addr)     Reserved for future system use
56          8       R7 Register (addr)         Cache for current_scope pointer
64          128     Stack Space (16 slots)     Reserved for future system stack
192         64      [Reserved for future SYS0 control data]
256         16      First Block Header         Payload begins
...         ...     [Free space for SYS0 alloc]
4088        8       Footer Marker              Bookend
──────────────────────────────────────────────────────────
Total:      4096 bytes
```

### 3.2 Block Header Structure

**Field Layout (16 bytes, kAlign-aligned):**

```c
struct sc_blk_header {
    addr next_off;      // Offset to next block header (0 = this is last)
    usize size;         // Total block size including header + footer
    sbyte flags;        // BLK_FLAG_FREE | BLK_FLAG_LAST | BLK_FLAG_FOOT
    sbyte _pad[7];      // Alignment to 16 bytes
}
```

**Flags:**
- `BLK_FLAG_FREE (0x01)`: Block is unallocated (available for reuse)
- `BLK_FLAG_LAST (0x02)`: This block is the last in the chain
- `BLK_FLAG_FOOT (0x04)`: Header marks the page footer (special marker)

**Invariants:**
- All headers are kAlign (16-byte) aligned
- Size always includes header (16) + payload + footer (8)
- next_off = 0 means this is the last block or chain end

### 3.3 Block Footer Structure

**Field Layout (8 bytes):**

```c
struct sc_blk_footer {
    addr magic;         // 0xDEADC0DE (validity marker)
    addr size_dup;      // Duplicate of header.size for validation
}
```

**Purpose:**
- Validates block integrity (no overflow/corruption)
- Allows reverse traversal: given footer, compute header offset as `(current - size_dup)`

---

## 4. Scopes & Arenas

### 4.1 Scope Concept (General)

A **scope** is a polymorphic allocator context accessed through the `Allocator` interface. All scopes implement:
- `alloc(size)` — Polymorphic allocation
- `dispose(ptr)` — Polymorphic deallocation
- `config(mask_type)` — Query policy/flags

Scopes are used via:
- **Implicit**: `Allocator.alloc(size)` — operates on current scope cached in R7
- **Explicit**: `Allocator.Scope.alloc(scope, size)` — operates on given scope

### 4.2 Arena Concept (Concrete Scope Type)

An **arena** is a concrete scope implementation with explicit lifecycle management and frame nesting support.

**Arena Characteristics:**
- Created with `Arena.create(policy, flags)` — immutable policy, mutable flags
- Owns one or more pages chained via sentinels
- Supports frame checkpoints for nested allocation scopes
- Can be destroyed or reset (subject to flag constraints)

**Arena Policy** (immutable at creation):
- `SCOPE_POLICY_DYNAMIC`: Auto-grows by chaining pages on overflow
- `SCOPE_POLICY_FIXED`: Pre-allocates all pages; returns NULL when exhausted

**Arena Flags** (mutable post-creation, unless FIXED policy):
- `SCOPE_FLAG_PROTECTED`: Blocks destroy/reset
- `SCOPE_FLAG_PINNED`: Blocks set_current and frame ops
- `SCOPE_FLAG_SECURE`: Blocks cross-arena move/copy

**SLB0** = Default user-space arena
- Instance of Arena with POLICY_DYNAMIC
- Default SCOPE_FLAG_SECURE (modifiable)
- Automatically initialized during bootstrap
- Used by `Allocator.alloc()` when set as current scope

### 4.3 Frame Concept (Temporary Scope)

A **frame** is a temporary checkpoint scope created within an arena. Frames enable nested bulk deallocation.

**Frame Characteristics:**
- Created via `Arena.begin_frame()` (not standalone)
- Saves allocation state: current page, bump pointer offset
- On `Arena.end_frame(frame)`, state restored → all frame-scoped allocations freed
- Frames are LIFO (last-created must be ended first)
- Frames do NOT create new pages; reuse arena's pages

**Frame Semantics:**
- No explicit policy needed (inherits from owning arena)
- Cannot be set as current scope (implicit when within frame)
- Can be nested: frame within frame within arena (for deep scopes)

**Example (pseudocode, Phase 3+):**
```c
arena work = Arena.create(SCOPE_POLICY_DYNAMIC, SCOPE_FLAG_SECURE);
Allocator.Scope.set(work);  // Set as current

frame outer = Arena.begin_frame();
  object buf1 = Allocator.alloc(256);  // Allocates from work arena
  frame inner = Arena.begin_frame();
    object buf2 = Allocator.alloc(128);  // Same arena, same page(s)
  Arena.end_frame(inner);  // buf2 freed; buf1 still valid
  object buf3 = Allocator.alloc(64);
Arena.end_frame(outer);  // buf1, buf3 freed
```

---

## 5. Data Structures

### 5.1 Register Structure (Virtual Registers in SYS0)

**Location**: First 64 bytes of SYS0 (offset 0)  
**Field Layout (64 bytes, 8 addr fields):**

```c
struct sc_registers_s {
  addr R0;        // SYS0 base address (offset resolution)
  addr R1;        // Reserved for future system use
  addr R2;        // Reserved for future system use
  addr R3;        // Reserved for future system use
  addr R4;        // Reserved for future system use
  addr R5;        // Reserved for future system use
  addr R6;        // Reserved for future system use
  addr R7;        // Current scope pointer (cache)
}
```

**Purpose:**
- **R7**: Cache for the currently active scope pointer. When `Scope.set_current(s)` is called, R7 is updated. This avoids repeated lookups from a registry.
- **R0**: SYS0 base address for offset resolution.
- **R1-R6**: Reserved for future system usage (e.g., thread-local contexts, ISR state).

**Invariants:**
- All registers are `addr` (= `uintptr_t`); they store address values, not C pointers
- R7 = 0 or NULL means no active scope
- Register access is O(1); base at offset 0 + register offset

### 5.2 Page Sentinel (Metadata)

**Location**: First 32 bytes of each 4K SLB page  
**Field Layout (32 bytes, kAlign-aligned):**

```c
struct sc_page_sentinel {
    addr next_page_off;     // Offset to next page in chain (0 = last page)
    addr bump_offset;       // Current bump pointer within this page (grows from sentinel end)
    usize scope_id;         // Owning scope ID (for cross-page navigation)
    sbyte flags;            // Page-level flags (reserved for future)
    sbyte page_index;       // Position in scope's page chain (0 = first)
    sbyte _pad[6];          // Alignment to 32 bytes
    addr reserved[2];       // Reserved for future metadata
}
```

**Invariants:**
- Sentinel is always at page offset 0
- bump_offset starts at 32 (end of sentinel) and grows upward
- bump_offset never exceeds kPageSize (4096)
- next_page_off points to sentinel of next page in chain, or 0 if this is last

### 5.3 Frame Marker (Checkpoint)

**Location**: Per-scope frame stack (array in SYS0)  
**Field Layout (8 bytes):**

```c
struct sc_frame_marker {
    addr page_off;          // Offset to page where checkpoint was taken
    addr bump_offset;       // Bump pointer value at time of begin
}
```

**Purpose**: Saves allocation state for nested frame scopes. On `Frame.end()`, bump pointer is restored to saved state, freeing all allocations made within the frame.

**Nesting**: Frames are per-scope; frame stack is separate per scope, allowing independent nesting contexts.

### 5.4 Scope Object (SYS0-allocated)

**Location**: SYS0 (allocated via reclaiming allocator)  
**Field Layout (variable size):**

```c
struct sc_scope {
    usize scope_id;         // Unique ID; used in page sentinels for reverse lookup
    sbyte policy;           // SCOPE_POLICY_DYNAMIC or SCOPE_POLICY_FIXED (immutable)
    sbyte flags;            // SCOPE_FLAG_* bitmask (mutable, except policy enforces immutability)
    addr first_page_off;    // Offset to first page's sentinel in memory
    addr current_page_off;  // Offset to current (last active) page
    usize page_count;       // Number of pages in chain
    string name;            // Scope name (e.g., "work_arena")
    scope_alloc_fn alloc;   // Polymorphic allocation function pointer
    scope_dispose_fn dispose; // Polymorphic disposal function pointer
    struct sc_frame_marker *frame_array;  // Array of frame markers (TBD allocation)
    usize frame_top;        // Current frame stack depth
    usize frame_cap;        // Capacity of frame array
}
```

**Concrete Instances:**
- **SYS0 Scope**: System allocator, reclaiming strategy, PROTECTED|PINNED flags
- **Arena Scope**: User-space allocator, bump strategy, DYNAMIC/FIXED policy, variable flags
- **Frame Scope**: Temporary checkpoint, inherits arena's pages/policy, LIFO nesting

**Stored In**: SYS0 registry (TBD: global array, linked list, or hash table)

---

## 6. Constants & Alignment

### 6.1 Fundamental Constants

```c
#define SYS0_PAGE_SIZE    4096           // Bootstrap page size (power-of-2)
#define SLB_PAGE_SIZE     4096           // User-space page size (equal to SYS0 for simplicity)
#define kAlign            16             // Alignment boundary (16 bytes, kAlign-multiple)

// Verify power-of-two at compile time
static_assert((SYS0_PAGE_SIZE & (SYS0_PAGE_SIZE - 1)) == 0, "SYS0_PAGE_SIZE must be power-of-2");
static_assert((SLB_PAGE_SIZE & (SLB_PAGE_SIZE - 1)) == 0, "SLB_PAGE_SIZE must be power-of-2");
```

### 6.2 SYS0 Layout Constants

```c
#define REGISTER_COUNT      8            // R0-R7 (virtual registers; R0=SYS0 base, R7=current scope)
#define REGISTER_SIZE       8            // bytes per register
#define REGISTER_RESERVED   (REGISTER_COUNT * REGISTER_SIZE)  // 64 bytes

#define STACK_SLOTS         16           // Stack slots
#define STACK_SIZE          8            // bytes per slot
#define STACK_RESERVED      (STACK_SLOTS * STACK_SIZE)  // 128 bytes

#define SYS0_RESERVED       256          // Total reserved (power-of-2): 64 + 128 + 64

#define FIRST_BLOCK_OFFSET  256          // First allocatable block header offset
#define LAST_FOOTER_OFFSET  (SYS0_PAGE_SIZE - sizeof(sc_blk_footer))  // 4088
```

### 6.3 Alignment Invariants

**Rule 1: All metadata is kAlign-aligned**
- Block headers at offsets ≡ 0 (mod 16)
- Page sentinels at offsets ≡ 0 (mod 16)
- Allocated payloads at offsets ≡ 0 (mod 16)

**Rule 2: Reserved regions are power-of-2 sized**
- SYS0_RESERVED = 256 (simplifies layout)
- All block sizes are kAlign multiples

**Rule 3: Memory state validation**
- Verify SYS0_PAGE_SIZE is power-of-2 (compile-time + runtime)
- Verify first block header is kAlign-aligned
- Verify last footer is kAlign-aligned (implicit: if FIRST_BLOCK_OFFSET + payload_size aligns)
- State flags returned as bitfield: `MEM_STATE_READY | MEM_STATE_ALIGN_SYS0 | MEM_STATE_ALIGN_HEADER | MEM_STATE_ALIGN_FOOTER`

---

## 7. API Surface (Allocator Interface)

### 7.1 Paradigm: Implicit vs. Explicit Scope

**Top-level `Allocator.*` operations** operate on the **current scope** (cached in R7):
- No scope parameter required
- Assume polymorphic dispatch to current scope's `alloc_fn` / `dispose_fn`

**`Allocator.Scope.*` operations** require **explicit scope parameter**:
- Exception: `Allocator.Scope.current()` (getter, no param)
- All setters and context-specific ops require scope handle

### 7.2 Top-Level Allocator (Current Scope)

```c
// Allocate size bytes from the CURRENT active scope (polymorphic)
object Allocator.alloc(usize size);
// Returns: pointer to allocated memory (kAlign-aligned), NULL if full (FIXED policy) or SYS0 OOM
// Side Effect: Routes to current_scope->alloc_fn via R7 caching

// Deallocate a block from the CURRENT scope (polymorphic)
void Allocator.dispose(object ptr);
// Returns: void
// Side Effect: Routes to current_scope->dispose_fn via R7 caching
```

### 7.3 Scope Management (Explicit Scope Operations)

```c
// Get the currently active scope from R7 register (getter, no parameters)
void *Allocator.Scope.current(void);
// Returns: current scope handle or NULL if no scope active

// Set the currently active scope in R7 register (requires scope parameter)
bool Allocator.Scope.set(void *scope_ptr);
// Parameters: scope_ptr = explicit scope to set as current
// Returns: true on success, false if scope_ptr has PINNED flag set
// Side Effect: Updates R7 register with new scope pointer

// Get configuration from explicit scope (requires scope parameter)
sbyte Allocator.Scope.config(void *scope_ptr, int mask_type);
// Parameters: 
//   scope_ptr = explicit scope to query
//   mask_type = SCOPE_POLICY or SCOPE_FLAG
// Returns: policy/flag value (0 on invalid)

// Allocate from explicit scope (requires scope parameter)
object Allocator.Scope.alloc(void *scope_ptr, usize size);
// Parameters: scope_ptr = explicit scope, size = bytes to allocate
// Returns: pointer to allocated memory, NULL if full or SYS0 OOM

// Deallocate to explicit scope (requires scope parameter)
void Allocator.Scope.dispose(void *scope_ptr, object ptr);
// Parameters: scope_ptr = explicit scope, ptr = pointer to free
// Returns: void
```

### 7.4 Scope Lifecycle (Phase 2+, NOT YET IMPLEMENTED)

```c
// Create a new scope with given name, initial size, and policy
scope Allocator.Scope.create(string name, usize initial_size, sbyte policy);
// PLANNED (Phase 2): Returns opaque scope handle (allocated in SYS0)
// Errors: NULL if SYS0 exhausted or parameters invalid

// Destroy a scope and free all associated pages/frames
integer Allocator.Scope.destroy(scope s);
// PLANNED (Phase 2): Returns OK (0) on success, ERR (-1) if PROTECTED flag set

// Reset scope to empty state (free all user allocations, keep metadata)
integer Allocator.Scope.reset(scope s);
// PLANNED (Phase 2): Returns OK on success, ERR if PROTECTED flag set

// Set flags on a scope (if policy allows)
integer Allocator.Scope.set_flags(scope s, sbyte flags);
// PLANNED (Phase 2): Returns OK on success, ERR if policy is FIXED (flags immutable)

// Get flags from a scope
sbyte Allocator.Scope.get_flags(scope s);
// PLANNED (Phase 2): Returns bitfield of SCOPE_FLAG_* flags
```

### 7.5 Cross-Scope Operations (Phase 4+, NOT YET IMPLEMENTED)

```c
// Move data from one scope to another (requires source and destination not SECURE)
integer Allocator.Scope.move_to(scope dst, object src_ptr, scope src);
// PLANNED (Phase 4): Returns OK and ptr written, ERR if SECURE flag blocks transfer

// Copy data from one scope to another
integer Allocator.Scope.copy_to(scope dst, object src_ptr, usize size, scope src);
// PLANNED (Phase 4): Returns pointer to copied data in dst, NULL if dst SECURE or insufficient space
```

---

## 8. Arena API (Concrete Scope with Frame Nesting)

### 8.1 Arena Lifecycle

```c
// Create a new arena with given policy and flags
arena Arena.create(sbyte policy, sbyte flags);
// Parameters:
//   policy = SCOPE_POLICY_DYNAMIC or SCOPE_POLICY_FIXED (immutable)
//   flags = combination of SCOPE_FLAG_* (mutable post-creation, unless policy=FIXED)
// Returns: arena handle (opaque), NULL if SYS0 exhausted
// Implementation Phase 2+

// Destroy an arena and free all associated pages
integer Arena.destroy(arena a);
// Returns: OK (0) on success, ERR (-1) if PROTECTED flag set
// Implementation Phase 2+

// Reset arena to empty state (frees allocations, keeps metadata)
integer Arena.reset(arena a);
// Returns: OK on success, ERR if PROTECTED flag set
// Implementation Phase 2+
```

### 8.2 Arena Frame Operations

```c
// Begin a new frame checkpoint in the arena
frame Arena.begin_frame(arena a);
// Parameters: a = arena to checkpoint
// Returns: frame handle (opaque), NULL if arena has PINNED flag set
// Implementation Phase 3+

// End the current frame (restore arena state to checkpoint)
integer Arena.end_frame(frame f);
// Parameters: f = frame to end (most recent for its arena)
// Returns: OK on success, ERR if frame's arena has PINNED flag set or frame not top of stack
// Implementation Phase 3+

// Get the owning arena of a frame
arena Arena.get_frame_arena(frame f);
// Parameters: f = frame to query
// Returns: arena handle that owns this frame
// Implementation Phase 3+
```

### 8.3 Arena Flag Management

```c
// Set flags on an arena (if policy allows)
integer Arena.set_flags(arena a, sbyte flags);
// Parameters: a = arena, flags = new flag values
// Returns: OK on success, ERR if policy is FIXED (flags immutable)
// Implementation Phase 2+

// Get flags from an arena
sbyte Arena.get_flags(arena a);
// Parameters: a = arena
// Returns: bitfield of SCOPE_FLAG_* flags
// Implementation Phase 2+
```

### 8.4 Frame Nesting Semantics

**Rule 1: Frames are arena-specific**
- Each frame is tied to one arena
- Frames from different arenas cannot be mixed

**Rule 2: LIFO (Last-In-First-Out)**
- `Arena.end_frame(f)` must match most recent `Arena.begin_frame()` in same arena
- Ending frames out-of-order returns ERR

**Rule 3: Bulk Deallocation**
- On `Arena.end_frame(f)`, all allocations since corresponding `Arena.begin_frame()` are freed
- Bump pointer restored to saved checkpoint
- Implementation: restore bump_offset to saved value; no per-allocation metadata needed

**Rule 4: Nested Frames**
- Frames can nest arbitrarily deep within an arena
- Inner frame end restores to inner checkpoint
- Outer frame end restores to outer checkpoint
- All allocations in inner frame freed when inner frame ends

**Example (Phase 3+):**
```c
arena work = Arena.create(SCOPE_POLICY_DYNAMIC, SCOPE_FLAG_SECURE);
Allocator.Scope.set(work);

frame f1 = Arena.begin_frame();         // Checkpoint 1: page 0, bump 32
  object buf1 = Allocator.alloc(256);   // Allocates from work arena
  frame f2 = Arena.begin_frame();       // Checkpoint 2: page 0, bump 288
    object buf2 = Allocator.alloc(128); // Reuses work arena pages
  Arena.end_frame(f2);                  // Restore to checkpoint 2: bump 288, buf2 freed
  object buf3 = Allocator.alloc(64);    // New allocation, bump now 352
Arena.end_frame(f1);                    // Restore to checkpoint 1: bump 32, buf1 and buf3 freed
```

---

## 9. Behavioral Rules & Constraints

### 9.1 Allocation Semantics

**SYS0 Reclaiming:**
- `Scope.free()` returns block to free list for reuse
- Fragmentation possible; not suitable for high-frequency alloc/free
- Designed for system object allocation (scopes, frames, page metadata)

**SLBn Bump:**
- No individual `free()`; only bulk reset via `Frame.end()` or `Scope.reset()`
- Deterministic: allocation time O(1), no fragmentation
- Policy determines growth: DYNAMIC chains pages, FIXED returns NULL when full

### 9.2 Flag Enforcement

| Flag | Operation | Behavior |
|------|-----------|----------|
| `PROTECTED` | `Scope.destroy()` or `Scope.reset()` | Returns ERR; scope cannot be destroyed/reset |
| `PINNED` | `Scope.set_current(s)` | Returns ERR; scope cannot become active |
| `PINNED` | `Frame.begin()` / `Frame.end()` | Returns ERR; cannot nest frames in this scope |
| `SECURE` | `Scope.move_to()` or `Scope.copy_to()` | Returns ERR; data cannot leave the scope |

### 9.3 Current Scope Rules

- **Default**: No scope is current at system start; must call `Scope.set_current()` to activate
- **ISR/Critical**: Set PINNED flag to prevent context switching from within ISR
- **Explicit Management**: Caller responsible for set_current; no implicit scope stack
- **Allocation Without Current**: Explicit scope handle required in all `Scope.alloc()` calls (no default)

### 9.4 Page Chaining

**Dynamic Scope (POLICY_DYNAMIC):**
1. Allocate first page on `Scope.create()`
2. On `Scope.alloc()` when current page full: allocate new page, chain via `next_page_off`, update `current_page_off`
3. Continue until SYS0 page metadata space exhausted

**Fixed Scope (POLICY_FIXED):**
1. On `Scope.create(size)`: calculate pages needed, allocate all upfront, chain them
2. On `Scope.alloc()`: bump within prealloc'd pages only; return NULL if no space

**Page Linkage**:
```c
// From scope metadata:
page_ptr = memory_base + first_page_off;    // Start of first page
// In page sentinel:
next_page = (next_page_off == 0) ? NULL : (memory_base + next_page_off);
```

---

## 10. Implementation Notes

### 10.1 Storage Locations

| Component | Location | Size | Allocated From |
|-----------|----------|------|-----------------|
| Virtual Registers (R0-R7) | SYS0 offset 0-63 | 64 bytes | Embedded in SYS0 |
| Stack (16 slots) | SYS0 offset 64-191 | 128 bytes | Embedded in SYS0 (reserved for future) |
| `sc_scope` struct | SYS0 registry (TBD) | ~80-120 bytes | SYS0 reclaiming |
| Frame stack array | SYS0 or embedded | 8 * frame_cap | SYS0 reclaiming |
| Page metadata (sentinel) | Start of each SLB page | 32 bytes | Embedded in page |
| **current_scope** (cached) | **SYS0 register R7** | **8 bytes (addr)** | **Cached, not allocated** |
| **sys0_base** (cached)     | **SYS0 register R0** | **8 bytes (addr)** | **Cached, not allocated** |
| SYS0 data | SYS0 pages | per-object | SYS0 reclaiming |
| SLBn user data | SLBn pages | per-alloc | SLBn bump allocation |

### 10.2 Initialization Sequence

1. **Bootstrap** (`init_memory_system()`):
   - Allocate SYS0 page (4K, sys page)
   - Verify SYS0_PAGE_SIZE is power-of-2
   - Initialize virtual registers (R0-R7) at offset 0:
     - R0 = SYS0 base address
     - R1-R6 = 0 (reserved for future use)
     - R7 = 0 (current scope cache starts null)
   - Initialize first block header at offset 256 (FREE, LAST flags)
   - Initialize footer at offset 4088 (magic = 0xDEADC0DE)
   - Mark memory state: MEM_STATE_READY | MEM_STATE_ALIGN_SYS0 | MEM_STATE_ALIGN_HEADER | MEM_STATE_ALIGN_FOOTER
   - **Phase 1 COMPLETE**: SYS0 initialized and tested

2. **Scope Creation** (`Allocator.Scope.create()` - Phase 2):
   - Allocate `sc_scope` struct in SYS0
   - Allocate initial page(s):
     - DYNAMIC: allocate 1 page
     - FIXED: pre-calculate pages needed, allocate all
   - Chain pages via `next_page_off` in sentinels
   - Initialize frame array (if nesting expected)
   - Set first_page_off, current_page_off, page_count
   - Allocate in SYS0 registry (global array/hash, TBD)
   - **Phase 2**: To be implemented

3. **Scope Activation** (`Allocator.Scope.set()` - Phase 1):
   - Check s is not PINNED; return false if true
  - Write scope handle to R7 (via register set)
   - Future polymorphic allocations default to this scope
   - **Phase 1 API**: Signature in place; functionality pending Phase 2 Scope.create()

4. **SLB (Slab) Implementation** (Phase 2):
   - SlabManager: Internal wrapper around PArray for slab registry
   - Each slab = page with page_sentinel at offset 0, bump allocator for payload
   - Borrow slot-reuse mechanics from SlotArray: ADDR_EMPTY sentinel, circular search for sparse reuse
   - Growth: 2x multiplier on PArray when slab registry fills
   - **Phase 2**: To be implemented with Scope lifecycle

### 10.3 Design for Real-Time Safety

**Deterministic Allocation:**
- SLBn bump allocation: O(1), no search
- Boundary checks: O(1)
- Page chaining: O(1) if current_page cached

**Bulk Deallocation:**
- Frame-based: O(1) restore (restore bump pointer only)
- No traversal or coalescing needed

**Predictability:**
- FIXED policy: pre-allocate all memory, allocation never fails (except SYS0 OOM for metadata)
- DYNAMIC policy: unbounded, but per-allocation cost constant

**Isolation:**
- Scope-based: data in one scope cannot corrupt another
- SECURE flag: prevent cross-scope leaks
- PROTECTED flag: prevent accidental deallocation

---

## 11. Testing Strategy (TDD)

### 11.1 Phase 1: Foundation (COMPLETE ✅)
- ✅ `test_sys0_initialized`: Header/footer bookends, magic marker
- ✅ `test_sanity_memory_alignments`: All alignment flags set
- ✅ `test_init_page_creates_single_free_block`: Block state and size
- ✅ Allocator interface refactored: `Scope` → `Allocator` (top-level), `Allocator.Scope.*` (explicit scope ops)
- ✅ Paradigm: No scope param for `Allocator.alloc/dispose`; all `Allocator.Scope.*` ops require scope param

### 11.2 Phase 2: Arena Lifecycle (Next)
- `test_arena_create_dynamic`: Create arena with DYNAMIC policy, allocate succeeds across page boundaries
- `test_arena_create_fixed`: Create arena with FIXED policy, allocates prealloc'd pages, fails when full
- `test_arena_destroy_succeeds`: Destroy non-protected arena
- `test_arena_destroy_protected_fails`: Destroy fails if PROTECTED flag set
- `test_arena_reset_clears_allocations`: Reset clears bump pointers across all pages
- **Implementation Note**: Arena lifecycle (create, destroy, reset); SLB slab implementation (SlabManager, page chaining)

### 11.3 Phase 3: Frame Nesting
- `test_frame_begin_end`: Save/restore bump offset
- `test_frame_nesting_two_levels`: Nested frames, LIFO behavior
- `test_frame_pinned_blocks`: PINNED arena returns ERR on frame_begin
- `test_frame_deep_nesting`: 10-level nesting, allocations freed correctly on unwind

### 11.4 Phase 4: Flag Enforcement
- `test_pinned_blocks_set_current`: Pinned scope cannot become current
- `test_protected_blocks_destroy`: Protected scope cannot be destroyed
- `test_secure_blocks_copy`: SECURE scope blocks copy_to

### 11.5 Phase 5: Cross-Scope Operations
- `test_move_to_success`: Move data between scopes
- `test_move_to_secure_fails`: SECURE scope blocks move_to
- `test_copy_to_allocates`: Copied data allocated in destination

### 11.6 Phase 6: Invariants Under Stress
- `test_alignment_maintained_after_alloc`: All state flags still set after allocation
- `test_page_chain_integrity`: Next pointers valid after page expansion
- `test_multiple_scopes_independent`: Allocations in different scopes don't interfere

---

## 12. Appendix: Layout Diagrams

### A.1 SYS0 Memory Layout

```
Offset (bytes)    Content                     Usage
─────────────────────────────────────────────────────
0                 R0-R6 Registers (addr)      Reserved for future system use
56                R7 Register (addr)          Current scope pointer (cache)
64                Stack (128 bytes)           Reserved for future system stack
                  [16 slots of 8 bytes each]
192               [Reserved] (64 bytes)       Future SYS0 control data
─────────────────────────────────────────────────────
256               Block Header (16 bytes)     First allocatable block
                  - next_off = 0
                  - size = 3824
                  - flags = FREE|LAST
272               [Payload - Free Space]      Available for SYS0 alloc
                  ...
4088              Block Footer (8 bytes)      Last bookend
                  - magic = 0xDEADC0DE
                  - size_dup = 3824
─────────────────────────────────────────────────────
Total: 4096 bytes
```

### A.2 SLBn Page Layout (Single Page)

```
Offset (bytes)    Content                     Usage
─────────────────────────────────────────────────────
0                 Page Sentinel (32 bytes)    Metadata
                  - next_page_off = 0
                  - bump_offset = 32
                  - scope_id = <scope_id>
                  - page_index = 0
32                [User Payload]              Allocatable via bump
                  ...
4096              [End of Page]               Natural boundary
─────────────────────────────────────────────────────
Total: 4096 bytes
```

### A.3 Multi-Page SLBn Scope (Dynamic Growth)

```
Page 0 (4K)                    Page 1 (4K)
┌──────────────────────┐       ┌──────────────────────┐
│ Sentinel             │       │ Sentinel             │
│ next_off → Pg1       │───────│ next_off = 0         │
│ bump = 1024          │       │ bump = 256           │
├──────────────────────┤       ├──────────────────────┤
│ User Data (992B)     │       │ User Data (224B)     │
│ [allocated]          │       │ [allocated]          │
│ [free]               │       │ [free]               │
└──────────────────────┘       └──────────────────────┘
Current Scope:
  first_page_off → Page 0
  current_page_off → Page 1 (where bump is)
  page_count = 2
```

---

## 15. Glossary

| Term | Definition |
|------|-----------|
| **Scope** | Abstract allocator interface; polymorphic type for generic operations. Concrete: SYS0, Arena, Frame. |
| **Arena** | Concrete scope type with explicit lifecycle; can have frames nested within. Created via `Arena.create()`. |
| **Frame** | Temporary scope nested within an arena; checkpoint for bulk deallocation. Created via `Arena.begin_frame()`. |
| **SLB0** | Default user-space arena; DYNAMIC policy, SECURE flags. Allocated from SYS0 during bootstrap. |
| **Policy** | Immutable arena property: DYNAMIC (auto-grow) or FIXED (prealloc) |
| **Flag** | Mutable scope property: PROTECTED, PINNED, or SECURE; controls access |
| **Bump Pointer** | Monotonically increasing offset within a page; allocation endpoint |
| **Page Chain** | Linked list of 4K pages; next pointers in sentinels |
| **Reclaiming** | Allocation strategy with free list; fragmentation possible (SYS0) |
| **Deterministic** | Allocation time O(1); no fragmentation; predictable latency |
| **Sentinel** | Metadata header at start of page; immutable during page lifetime |

---

## 15. Design Rationale Notes

### Current Scope Caching (R7 vs. Separate Field)

**Why R7 instead of a dedicated field at offset 192-199?**

The current_scope pointer is cached in register R7 rather than a separate reserved field because:

1. **Semantic Correctness**: Registers exist to cache frequently-accessed state. R7 is dedicated to the most-active allocator context while keeping R0-R6 free for future system/runtime use.

2. **Architectural Alignment**: When register/stack management is fully implemented later, R7 will already be correctly integrated; no refactoring needed. Lower registers remain available for ISR/thread metadata.

3. **Solves the Chicken-Egg Problem**: We defer the full register struct implementation (which would require thread/ISR management) but use a minimal version now (8 addr fields) to establish the caching pattern.

4. **Frees Reserved Space**: By using R7, we avoid consuming a separate offset (192-199) and keep the 192-255 region truly reserved for future SYS0 control data.

**Implementation Note**: Registers start at 0; R7 is set to the current scope during bootstrap. When ISR/context management arrives, the remaining registers will gain purpose without architectural disruption.

---

## 14. Implementation Status & Concerns

### 14.1 Phase 1: Complete ✅

**What's Implemented:**
- SYS0 bootstrap allocator (4K reclaiming)
- Virtual register R7 for current scope caching
- Block header/footer with splitting and alignment
- Allocator interface refactor (Scope → Allocator)
- Paradigm: implicit scope for `Allocator.*`, explicit scope for `Allocator.Scope.*`
- 9/9 bootstrap tests passing; valgrind clean (0 bytes definitely lost)

**What's NOT Implemented (Phase 1):**
- Scope lifecycle (create, destroy, reset) — deferred to Phase 2
- SLB slab allocator — deferred to Phase 2
- Frame nesting — deferred to Phase 3
- Flag enforcement (PROTECTED, PINNED, SECURE) — runtime checks needed

### 14.2 Design Decisions & Rationale

#### Allocator Interface Paradigm (Phase 1)

**Decision**: Top-level `Allocator.alloc(size)` operates on current scope (no scope parameter); `Allocator.Scope.*` operations require explicit scope.

**Rationale**:
- **Simplicity**: Most user code just calls `Allocator.alloc()`; implicit use of current scope (R7) reduces parameter count
- **Consistency**: Matches `Allocator.Frame.*` (future) where `Frame.begin()` uses current scope implicitly
- **Clarity**: Nested `.Scope` namespace signals "explicit scope required"
- **Flexibility**: Power users can still call `Allocator.Scope.alloc(scope_ptr, size)` for explicit control

#### R7 Register for Current Scope Caching

**Decision**: Current scope pointer cached in R7 register (virtual register at offset 56-63) rather than dedicated SYS0 field.

**Rationale**:
- **Register Semantics**: Registers are designed for frequently-accessed state; dedicating R7 isolates the "current context" while reserving R0-R6 for future system use.
- **Future-Proof**: When ISR/thread contexts arrive, R7 will already be integrated; no architectural refactor needed
- **Clean Semantics**: Distinguishes "currently active" (R7) from "stored metadata" (SYS0 fields), enabling future per-thread or per-ISR contexts

#### Scope Parameter Requirement for `config()`

**Decision**: `Allocator.Scope.config(scope, mask_type)` requires explicit scope; no "config of current scope" at top level.

**Rationale**:
- **Consistency**: All `Allocator.Scope.*` operations except `.current()` require scope param
- **Predictability**: Prevents accidental query of wrong scope
- **Future**: Top-level `Allocator.get_config(mask_type)` could be added later as convenience wrapper if needed

### 14.3 Known Limitations & Open Questions

#### Scope Registry (Phase 2)

**Question**: How are scopes (arenas) stored and looked up? Options:
1. Global static array (fixed max arenas)
2. Linked list in SYS0 (fragmentation risk)
3. Hash table in SYS0 (complexity; may require allocation)
4. Single current scope (R7 only; no registry)

**Current Assumption**: Multiple arenas supported via registry (TBD structure); only current arena cached in R7.

#### Frame Stack Storage (Phase 3)

**Question**: Where does each arena's frame stack live? Options:
1. Embedded in arena struct (limited frame depth)
2. Allocated in SYS0 (fragmentation)
3. Preallocated array in SYS0 during arena creation

**Current Assumption**: Allocated in SYS0 per arena during `Arena.create()`.

#### SLB (Slab) Architecture (Phase 2)

**Question**: Slab = single fixed page or dynamic multi-page container?

**Current Design**:
- Each slab/page = single 4K page with page_sentinel at offset 0, bump allocator for payload
- SlabManager (PArray-based) registry manages multiple slabs/pages within an arena
- Arena can own multiple pages chained via `next_page_off`
- Growth: When page fills, allocate new page, chain via sentinel, update current_page_off
- Slot reuse: Borrow ADDR_EMPTY sentinel + circular search from SlotArray (future, Phase 4+)

**Concern**: Performance of multi-page allocation. If app frequently allocates > 4KB, will create many pages and potentially fragment arena. Mitigation: Pre-allocate larger arenas or implement variable-size pages (Phase 4+).

#### Thread/ISR Safety (Phase 4+)

**Current Design**: Single R7 for current scope; no per-thread or per-ISR contexts.

**Concern**: Multi-threaded apps will have race conditions on R7. Solution: Extend R0-R7 registers to thread-local or ISR-stack contexts (Phase 4+).

#### SLB0 Default Arena

**Design**: SLB0 is an Arena instance with:
- Policy: SCOPE_POLICY_DYNAMIC
- Flags: SCOPE_FLAG_SECURE (default, can be modified)
- scope_id: 1 (reserved)
- Allocated from SYS0 during bootstrap

**Behavior**: After bootstrap, `Allocator.alloc()` operates on SLB0 by default (current scope in R7).

### 14.4 Deferred Implementation

The following are **planned but NOT implemented**:

| Feature | Phase | Status |
|---------|-------|--------|
| Arena lifecycle (create/destroy/reset) | 2 | 🔜 Planned |
| SLB slab/page allocator | 2 | 🔜 Planned |
| Arena.set_flags / get_flags | 2 | 🔜 Planned |
| Frame nesting (Arena.begin_frame/end_frame) | 3 | 🔜 Planned |
| PINNED/PROTECTED/SECURE flag enforcement | 2-4 | 🔜 Planned |
| Cross-arena move/copy | 4 | 🔜 Planned |
| Arena registry (TBD structure) | 2 | 🔜 TBD |
| Variable-size pages/slabs | 4+ | 🔜 Future |
| Thread-local arena contexts | 4+ | 🔜 Future |

---

## 14. Version History

| Version | Date | Changes |
|---------|------|---------|
| 1.2 | 2026-01-22 | Updated API: Scope → Allocator (top-level), Allocator.Scope.* (explicit scope). Added config(scope, mask_type) signature. Documented paradigm, design rationale, known limitations. Phase 1 marked COMPLETE. |
| 1.1 | 2026-01-20 | Register-based R7 caching for current_scope; moved from offset 192-199 field to R7 register |
| 1.0 | 2026-01-20 | Initial design document; dual allocator architecture, scope policies/flags, frame nesting, SYS0/SLBn layouts |

