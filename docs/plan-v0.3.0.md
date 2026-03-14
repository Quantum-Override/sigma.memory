# Sigma.Memory v0.3.0 — Trusted Subsystem Registration

**Date:** March 11, 2026
**Target Version:** 0.3.0
**Theme:** Trusted Subsystem Registration + Sigma.Tasking Integration
**Methodology:** TDD (Test-Driven Development) — Non-Negotiable

---

## Strategic Context

Sigma.Memory is on a deliberate path toward becoming the **Ring 0 memory component of Sigma.OS**
— the lowest-level, most trusted allocator in the system stack. Every design decision in v0.2.x
has moved in this direction: external metadata, page-as-payload, opaque scopes, virtual register
file, and the scope chain. v0.3.0 takes the next step by introducing a **trusted subsystem
registration** model — a mechanism by which privileged Sigma.X components can formally register
with Memory, receive dedicated resources, and operate with elevated capability while Memory
retains ultimate ownership of all physical pages.

**Sigma.Tasking** is the first trusted registrant. Its role in the Sigma.X architecture is
analogous to a **Ring 1 task scheduler**: it sits directly above Memory, is granted its own
managed region, and uses that region to run fibers and tasks without burdening the user-space
scope table. Together, Memory (Ring 0) and Tasking (Ring 1) form the foundation on which all
higher-level Sigma.X components stand.

> **Future Note:** As the architecture approaches OS-level implementation, registration fixtures
> and larger slab reservations will move into the **SYS0-DAT region** as permanent bootstrap
> allocations, alongside the scope table and node table. At that stage, trusted subsystem headers
> become first-class SYS0 citizens rather than dynamically mmap'd pages. The design chosen here
> anticipates this transition — the `sc_trusted_header` layout is intentionally compatible with
> being embedded in SYS0-DAT in a later version.

---

## Problem Statement

### The 16-Scope Ceiling

The user-space scope table supports exactly 16 scopes: SYS0 (index 0), SLB0 (index 1), and 14
user arenas (indices 2–15). This limit is **structural** — it is encoded in the SYS0 page
geometry:

```
Offset 64: SlabTable[16] × 8 bytes = 128 bytes (fixed)
```

For general application use, 14 concurrent arenas is generous. For a fiber-based task scheduler,
it is a **showstopper**. Each fiber needs its own bump arena (execution isolation, independent
stack, clean disposal on yield/exit). A scheduler managing 100 in-flight fibers cannot put each
arena into `scope_table` — there are only 14 slots, and they are also needed for application
subsystem arenas (string pools, parser arenas, scratch regions, etc.).

### pthread Cost vs. Fiber Goal

The alternative — `pthreads` — carries a fixed stack overhead of roughly 8 MB per thread (default
kernel mmap). For a high-concurrency fiber model targeting thousands of concurrent tasks at 4 KB
stacks each, `pthreads` is architecturally incompatible. Fibers are cooperative, lightweight, and
explicitly sized; their memory must be controllable at the allocator level.

### The Structural Answer

Tasking's fiber arenas must not consume `scope_table` slots. They need:
1. A dedicated managed region, separate from the 16-scope table.
2. Memory's page supply and reclamation machinery.
3. Protection from accidental disposal by code running in user scope.
4. A stable registration token — a pointer that survives arena growth and can be passed between
   scheduler and Memory.

---

## Design Overview

### The Three Layers

```
┌─────────────────────────────────────────────────────────┐
│                  User Application                       │
│    Allocator.alloc / Arena.create / Frame.begin         │
├─────────────────────────────────────────────────────────┤
│              Sigma.Tasking  (Ring 1)                    │
│    task_create / fiber_spawn / task_yield               │
│    Allocator.Trusted.alloc_arena / free_arena           │
├─────────────────────────────────────────────────────────┤
│              Sigma.Memory  (Ring 0)                     │
│    SYS0  |  SLB0  |  scope_table[0..15]                 │
│    Trusted Slab  |  Trusted Control Page (R3)           │
└─────────────────────────────────────────────────────────┘
```

### The Registration Handshake

When Sigma.Tasking starts, it calls a single registration point:

```c
object sys_page = Allocator.Trusted.register("Tasking", 8 * 1024 * 1024, TRUSTED_GROWTH_AUTO);
```

Memory responds by:
1. Finding the next free register slot (R3 on first call, R4 on second, etc.).
2. `mmap`-ing an 8 KB **SYS control page** for Tasking.
3. Storing `R3 → sys_page` (stable; never changes over Tasking's lifetime).
4. `mmap`-ing the requested initial slab (8 MB by default).
5. Tracking the slab in SLB0's B-tree with `NODE_FLAG_PROTECTED` — preventing any
   `Allocator.dispose()` path from touching it.
6. Writing a `sc_trusted_header` at the top of the control page.
7. Returning the control page pointer to Tasking as its **registration token**.

Tasking stores this pointer once at startup. It is opaque to the rest of the world — only
the trusted interface (`Allocator.Trusted.*`) accepts it as a valid handle.

### The Control Page

```
┌──────────────────────────────────────────────┐  offset 0
│  sc_trusted_header (≈56 bytes)               │
│  ─ slab_base           addr    (8)           │
│  ─ slab_size           usize   (8)           │  Memory writes
│  ─ slab_used           usize   (8)           │  these fields
│  ─ arena_count         uint32  (4)           │
│  ─ slot                uint8   (1)           │
│  ─ growth_policy       uint8   (1)           │
│  ─ _pad                uint16  (2)           │
│  ─ growth_cb           fn ptr  (8)           │
│  ─ name[16]            char[]  (16)          │
├──────────────────────────────────────────────┤  offset 56
│  Free-list metadata (Memory-managed)         │
│  384 bytes — ≤16 nodes hard limit (v0.3.0)   │
├──────────────────────────────────────────────┤  offset 440
│  Subsystem scratch space (~7.6 KB)           │
│  (Tasking owns this region entirely)         │
│  ─ scheduler state                           │
│  ─ fiber registry                            │
│  ─ run queue metadata                        │
│  ─ anything Tasking needs                    │
└──────────────────────────────────────────────┘  offset 8192
```

Memory writes only the header and free-list. Everything below offset 440 is **Tasking's
internal scratch space** — Memory never reads or writes it. This gives Tasking a stable,
zero-allocation home for scheduler bookkeeping without needing any `scope_table` slot.

### The Slab as a Sub-Allocator's Domain

The slab is a contiguous mmap'd region sized in multiples of `TRUSTED_SLAB_GROW_INCREMENT`
(default 8 MB). Conceptually it is Tasking's private memory pool:

```
Slab (8 MB initial)
├── [4 KB] fiber-0 arena   ← alloc_arena(sys_page, 4096)
├── [4 KB] fiber-1 arena
├── [8 KB] fiber-2 arena   ← custom size
├── [4 KB] fiber-3 arena
├── [free — 8176 KB]       ← returned by free_arena as fibers exit
└── ...
```

Internally, Memory maintains a **first-fit free-list** over the slab. Free-list nodes (each
`{addr base; usize size; addr next_off;}`) live in the control page free-list section — no extra
mmap needed for typical workloads. Arena sizes are coarse (multiples of `TRUSTED_MIN_ARENA_SIZE`,
default 4 KB), so the free-list remains short even under heavy churn.

When `free_arena` is called, Memory coalesces adjacent free blocks — the same mechanism used by
SYS0's first-fit reclaimer. Fragmentation cannot accumulate across fiber lifetimes.

### Growth Policy

At registration, Tasking declares how it wants slab exhaustion handled:

| Policy | Constant | Behaviour |
|--------|----------|-----------|
| Automatic | `TRUSTED_GROWTH_AUTO` | Memory grows the slab transparently on the next `alloc_arena` that would otherwise fail. Tasking sees no error. |
| Callback | `TRUSTED_GROWTH_CALLBACK` | Memory calls `growth_cb(sys_page, current_size, needed)` before growing. Tasking returns `OK` to permit growth or `ERR` to deny it (alloc returns `NULL`). |
| Manual | `TRUSTED_GROWTH_MANUAL` | Memory returns `NULL` on exhaustion. Tasking must call `Trusted.grow(sys_page)` explicitly to expand the slab. |

Growth adds exactly `TRUSTED_SLAB_GROW_INCREMENT` (8 MB) via `mremap(MREMAP_MAYMOVE)`. If the
slab moves, Memory updates `header->slab_base` atomically.

**Slab move and per-fiber arena references (R-01):**  
`alloc_arena` returns an absolute address for immediate use. Tasking must **not** store that
absolute address per-fiber — after a slab relocation it would be stale. Instead, Tasking stores
the arena as an **offset from `slab_base`** at spawn time:

```c
usize arena_offset = (usize)((byte *)arena_abs - (byte *)hdr->slab_base);
```

The absolute address is recomputed on demand as `hdr->slab_base + f->arena_offset`. Since
`hdr` is the stable control page pointer (R3; never moves), this is always correct after any
slab relocation — no notification protocol required between Memory and Tasking.

---

## General Registration Model

The trusted registration API is intentionally **not Tasking-specific**. Any Sigma.X component
that needs a dedicated page-backed region and special allocator access can register. The design
supports up to `MAX_TRUSTED_SUBSYSTEMS` (4) concurrent registrants, mapped to free register slots
R3–R6.

| Slot | Register | Reserved For |
|------|----------|--------------|
| 0 | R3 | Sigma.Tasking (first registrant) |
| 1 | R4 | Future: Sigma.IO, Sigma.Net, etc. |
| 2 | R5 | Future |
| 3 | R6 | Future |

A subsystem that attempts to register when all slots are taken receives `NULL`. A subsystem
that deregisters while live arenas remain in its slab receives `ERR` — Memory will not force-
reclaim in-use regions.

---

## API Specification

### Growth Policy Type

```c
// include/internal/memory.h

typedef enum sc_trusted_growth {
    TRUSTED_GROWTH_AUTO     = 0,
    TRUSTED_GROWTH_CALLBACK = 1,
    TRUSTED_GROWTH_MANUAL   = 2,
} sc_trusted_growth;
```

### Control Page Header

```c
// include/internal/memory.h

typedef struct sc_trusted_header {
    addr     slab_base;          // 8: base address of current slab (Memory updates on grow)
    usize    slab_size;          // 8: total current slab size in bytes
    usize    slab_used;          // 8: bytes currently allocated as live arenas
    uint32_t arena_count;        // 4: number of live arenas
    uint8_t  slot;               // 1: register slot (3–6) assigned to this subsystem
    uint8_t  growth_policy;      // 1: sc_trusted_growth value
    uint16_t _pad;               // 2: alignment
    // NOTE (R-04): growth_cb is valid in user-space only.
    // At Ring 0 promotion this field becomes an event sink ID; the callback model
    // is replaced by a Ring 1 inbox notification posted by Memory, consumed by the
    // Ring 1 scheduler on next dispatch. TRUSTED_GROWTH_CALLBACK behaviour changes
    // accordingly. See Sigma.OS Ring 0 migration guide.
    int    (*growth_cb)(object sys_page, usize current_size, usize needed);  // 8
    char     name[16];           // 16: subsystem name (null-terminated)
} sc_trusted_header;             // 56 bytes total
```

### Public Interface

```c
// include/memory.h

typedef struct sc_allocator_trusted_i {
    /**
     * @brief Register a trusted subsystem with Memory.
     *
     * Allocates an 8KB SYS control page and a contiguous slab of `initial_size` bytes.
     * Stores a pointer to the control page in the next free register slot (R3..R6).
     * The returned pointer is the subsystem's registration token — it is stable for
     * the lifetime of the registration.
     *
     * @param name         Subsystem name (max 15 chars, null-terminated).
     * @param initial_size Initial slab size in bytes (rounded up to page boundary).
     * @param policy       Growth policy (AUTO, CALLBACK, or MANUAL).
     * @return             Pointer to the 8KB SYS control page, or NULL on failure.
     */
    object (*register_sys)(const char *name, usize initial_size, sc_trusted_growth policy);

    /**
     * @brief Allocate an arena from the subsystem's slab.
     *
     * Carves `size` bytes (rounded up to TRUSTED_MIN_ARENA_SIZE) from the slab free-pool.
     * If the slab is exhausted, applies the registered growth policy.
     *
     * @param sys_page  Registration token returned by register_sys.
     * @param size      Arena size in bytes (minimum TRUSTED_MIN_ARENA_SIZE).
     * @return          Base pointer of the arena, or NULL if growth is denied/not possible.
     */
    object (*alloc_arena)(object sys_page, usize size);

    /**
     * @brief Return an arena to the slab free-pool.
     *
     * Inserts the region back into the free-list and coalesces adjacent free blocks.
     * Caller is responsible for ensuring no live pointers into the arena remain.
     *
     * @param sys_page  Registration token.
     * @param arena     Base pointer previously returned by alloc_arena.
     * @param size      Size of the arena (must match the value given to alloc_arena).
     * @return          OK on success, ERR if the region is unknown or mis-sized.
     */
    int (*free_arena)(object sys_page, object arena, usize size);

    /**
     * @brief Manually grow the slab by TRUSTED_SLAB_GROW_INCREMENT bytes.
     *
     * Only meaningful when growth_policy == TRUSTED_GROWTH_MANUAL.
     * Safe to call from CALLBACK policy handlers as well.
     *
     * @param sys_page  Registration token.
     * @return          OK if mremap succeeds, ERR otherwise.
     */
    int (*grow)(object sys_page);

    /**
     * @brief Deregister the subsystem, releasing the slab and control page.
     *
     * Returns ERR (does not deregister) if arena_count > 0. The caller must
     * free all live arenas before deregistering.
     * Clears the assigned register slot (R3..R6) on success.
     *
     * @param sys_page  Registration token.
     * @return          OK on success, ERR if live arenas remain.
     */
    int (*deregister)(object sys_page);
} sc_allocator_trusted_i;
```

The interface is exposed as `Allocator.Trusted` on the global `sc_allocator_i` instance.

---

## How Sigma.Tasking Uses the Interface

### Registration (Tasking Startup)

```c
// Sigma.Tasking init — called once at subsystem startup
static object _sys_page = NULL;

void tasking_init(void) {
    _sys_page = Allocator.Trusted.register_sys("Tasking", 8 * 1024 * 1024, TRUSTED_GROWTH_AUTO);
    if (_sys_page == NULL) {
        // fatal: Memory refused registration (all slots taken or mmap failed)
        abort();
    }
    // The upper half of the control page is ours — init scheduler state there
    task_scheduler_init(_sys_page);
}
```

`_sys_page` is Tasking's only external handle into this system. It points to R3's target.
Memory knows `R3 → _sys_page → {slab_base, ...}`.

### Fiber Spawn

**`fiber_t` location (R-02):** `fiber_t` structs live in the **control page scratch region**
(`TRUSTED_SCRATCH_OFFSET` onwards) — never inside the arena. This avoids any use-after-free
when the arena is returned to the slab free-pool. The scratch space (~7.6 KB) is sufficient
for a fixed-capacity fiber registry at reasonable concurrency.

```c
// Sigma.Tasking — spawn a new fiber
fiber_t *fiber_spawn(fiber_fn fn, void *arg, usize stack_size) {
    usize arena_size = stack_size == 0 ? TASKING_DEFAULT_STACK : stack_size;
    // Minimum 4KB; align to page boundary
    if (arena_size < TRUSTED_MIN_ARENA_SIZE)
        arena_size = TRUSTED_MIN_ARENA_SIZE;

    object arena_abs = Allocator.Trusted.alloc_arena(_sys_page, arena_size);
    if (arena_abs == NULL)
        return NULL;  // policy denied growth or truly exhausted

    // Store offset, not absolute address — safe across slab mremap relocations (R-01)
    sc_trusted_header *hdr = (sc_trusted_header *)_sys_page;
    usize arena_offset = (usize)((byte *)arena_abs - (byte *)hdr->slab_base);

    // fiber_t allocated from scratch space, not from the arena (R-02)
    fiber_t *f = tasking_scratch_alloc_fiber(_sys_page);
    f->arena_offset = arena_offset;
    f->arena_size   = arena_size;
    tasking_fiber_init(f, arena_abs, fn, arg);
    return f;
}
```

Each fiber receives an isolated arena. Memory carved it from the slab. Tasking's scheduler
holds the `{arena_offset, arena_size}` pair in the per-fiber struct (in scratch space)
— needed for reclamation. Memory is not involved in scheduling, yielding, or context
switching. Those are purely Tasking concerns.

### Fiber Exit / Reclamation

```c
// Sigma.Tasking — clean up after a fiber completes
void fiber_destroy(fiber_t *f) {
    // Recompute absolute address from offset — correct even after slab relocation (R-01)
    sc_trusted_header *hdr = (sc_trusted_header *)_sys_page;
    object arena_abs  = (byte *)hdr->slab_base + f->arena_offset;
    usize  arena_size = f->arena_size;

    // Return the arena to Memory's slab free-pool before touching f (f is in scratch, not
    // in the arena — so no use-after-free here; R-02)
    int rc = Allocator.Trusted.free_arena(_sys_page, arena_abs, arena_size);
    if (rc != OK) {
        // Should not happen if f was properly spawned; treat as fatal in debug builds
    }
    tasking_scratch_free_fiber(_sys_page, f);  // returns slot in scratch fiber registry
}
```

Memory handles coalescing. If the freed arena is adjacent to another free block, they merge.
Tasking does not need its own free-list.

### Scheduler Scratch Space

The scheduler's internal data (run queues, fiber registry, wake timers) lives in the control
page scratch area — the ~7.6 KB region below the Memory-managed free-list:

```c
// Sigma.Tasking — internal layout of its control page scratch area
typedef struct task_scheduler_state {
    uint32_t fiber_count;
    uint32_t next_fiber_id;
    // run queue head/tail, timer heap, etc.
    // fits comfortably in ~7KB
} task_scheduler_state;

static void task_scheduler_init(object sys_page) {
    // Cast past the Memory-managed header + free-list section
    task_scheduler_state *sched =
        (task_scheduler_state *)((byte *)sys_page + TRUSTED_SCRATCH_OFFSET);
    memset(sched, 0, sizeof(task_scheduler_state));
}
```

`TRUSTED_SCRATCH_OFFSET` is a constant defined in `include/config.h` — the byte offset
within the control page where subsystem scratch space begins.

---

## Internal Implementation Notes

### Protected Slab in SLB0

The slab is tracked in SLB0's B-tree as a single large allocation with a new protection flag:

```c
// New flag for sc_node.info, bits 11-15 (verify available bits before assigning)
#define NODE_FLAG_PROTECTED   0x0800   // bit 11 — protected allocation; dispose returns ERR
```

Any call to `slb0_dispose()` or `scope_dispose()` that encounters a node with this flag set
returns `ERR` without touching the allocation. Normal application code cannot free a trusted slab
by accident, regardless of pointer aliasing.

### Free-list Node Layout

Free-list nodes live in the control page at `TRUSTED_FREELIST_OFFSET` (after the header):

```c
typedef struct sc_slab_free_node {
    addr   base;        // 8: start of free region
    usize  size;        // 8: size of free region in bytes
    uint32_t next_off;  // 4: offset from freelist start to next node (0 = end)
    uint32_t _pad;      // 4
} sc_slab_free_node;    // 24 bytes
```

At 24 bytes per node, the 384-byte free-list section holds exactly `TRUSTED_FREELIST_MAX_NODES`
(16) nodes. **This is a hard limit for v0.3.0**: `free_arena` returns `ERR` when the free-list
is full. In practice, coalescing keeps the list short for similarly-sized arenas; workloads with
highly irregular sizes should use `TRUSTED_GROWTH_AUTO` to keep the slab spacious. A two-page
free-list design is deferred to v0.4.0 (see R-03 in Appendix). Test `TAS-07` covers the
boundary: `free_arena` must return `ERR` at node capacity and leave the allocator consistent.

### mremap and slab_base Stability

When `trusted_grow_slab()` calls `mremap(MREMAP_MAYMOVE)`, the slab may relocate. Memory:

1. Updates `header->slab_base` to the new address.
2. Updates the SLB0 B-tree node's `start` address for the slab allocation.
3. Recomputes all free-list node `base` values (they are absolute addresses; a full scan
   of the free-list is required — O(n) over at most `TRUSTED_FREELIST_MAX_NODES` = 16 entries).
4. Returns `OK` to the caller.

Tasking reconstructs absolute arena addresses on demand via `hdr->slab_base + f->arena_offset`
(see Fiber Spawn section). Because `hdr` is the stable control page pointer and `arena_offset`
is slab-relative, all per-fiber references remain valid after any relocation with no notification
required between Memory and Tasking. (Resolution of R-01.)

---

## Implementation Plan

### Phase 1 — Config & Types (include/)

**Files:** `include/config.h`, `include/internal/memory.h`, `include/memory.h`

- Add constants: `MAX_TRUSTED_SUBSYSTEMS`, `TRUSTED_DEFAULT_SLAB_SIZE`,
  `TRUSTED_SLAB_GROW_INCREMENT`, `TRUSTED_MIN_ARENA_SIZE`, `TRUSTED_SCRATCH_OFFSET`,
  `TRUSTED_FREELIST_OFFSET`, `TRUSTED_FREELIST_MAX_NODES`
- Add `NODE_FLAG_PROTECTED` (verify available bit in `sc_node.info`)
- Add `sc_trusted_growth` enum
- Add `sc_trusted_header` struct
- Add `sc_slab_free_node` struct
- Add `sc_allocator_trusted_i` interface definition
- Add `.Trusted` field to `sc_allocator_i`

### Phase 2 — Registration & Control Page (src/memory.c)

TDD cycle per function:

**TSR-01**: `trusted_register_impl` — write test first
- Scan R3–R6 for free slot
- `mmap` control page → initialize `sc_trusted_header`
- `mmap` slab → store in header
- Register slab in SLB0 B-tree with `NODE_FLAG_PROTECTED`
- Store control page in R-slot
- Return control page

**TSR-02**: `trusted_deregister_impl`
- Check `arena_count == 0` (ERR if not)
- `munmap` slab (remove from SLB0 B-tree first)
- `munmap` control page
- Clear R-slot

### Phase 3 — Slab Sub-allocator (src/memory.c)

**TAS-01**: `trusted_alloc_arena_impl`
- First-fit scan of free-list
- On hit: carve block, update free-list, update `slab_used` + `arena_count`
- On miss: apply growth policy; retry after grow

**TAS-02**: `trusted_free_arena_impl`
- Validate `arena` is within slab bounds
- Insert into free-list
- Coalesce with adjacent free blocks
- Update `slab_used`, decrement `arena_count`

### Phase 4 — Growth (src/memory.c)

**TGR-01**: `trusted_grow_slab_impl`
- `mremap(slab_base, old_size, new_size, MREMAP_MAYMOVE)`
- Update `header->slab_base` on move
- Update SLB0 B-tree node `start`
- Recompute free-list `base` values
- Add new free block to free-list (the grown region)
- Update `slab_size`

**TGR-02**: Growth policy dispatch in `trusted_alloc_arena_impl`

### Phase 5 — Protection Guard (src/memory.c)

**TPG-01**: Add `NODE_FLAG_PROTECTED` check to `slb0_dispose` path
- If node carries the flag, return `ERR` before any deallocation

### Phase 6 — Tests

| File | Tests | Coverage |
|------|-------|----------|
| `test/unit/test_trusted_registration.c` | TSR-01..05 | Register, R-slot assignment, 5th call NULL, deregister lifecycle, name collision |
| `test/unit/test_trusted_alloc.c` | TAS-01..07 | alloc/free round-trip, coalescing, protection guard, alloc after free, NULL on exhaustion (MANUAL), over-size request, **TAS-07: `free_arena` ERR at free-list capacity** |
| `test/unit/test_trusted_growth.c` | TGR-01..06 | AUTO grow, CALLBACK allow, CALLBACK deny, MANUAL explicit grow, grow after move (offset-based refs valid post-mremap), live arenas survive grow |
| `test/integration/test_trusted_tasking_sim.c` | TSM-01..04 (**memory simulation — no fiber execution**) | 100 simulated fiber structs, arenas allocated with mixed sizes, sentinel values written + verified, freed in random order, coalescing verified, all policies exercised, valgrind clean. No context switching. Full fiber execution tests are Phase 4 of the Sigma.Tasking plan, gated on the assembly context-switch layer. |

---

## Acceptance Criteria

- [ ] `Allocator.Trusted.register_sys("Tasking", 8*1024*1024, TRUSTED_GROWTH_AUTO)` returns non-NULL
- [ ] R3 contains the address of the returned control page
- [ ] 5th `register_sys` call returns NULL
- [ ] `Allocator.dispose(slab_pointer)` returns ERR (protection flag)
- [ ] 100 `alloc_arena` + `free_arena` cycles produce zero bytes leaked (valgrind)
- [ ] All three growth policies tested and passing
- [ ] `deregister` with live arenas returns ERR
- [ ] `deregister` after all arenas freed: R-slot cleared, no leak

---

## Backlog Updates

This plan resolves **FT-01** (thread-safety hooks for Sigma.Tasking) as **superseded** —
the hook model has been replaced by the trusted registration model, which is both richer and
architecturally cleaner. FT-01 should be closed with a note pointing here.

The 16-scope ceiling (not a bug, not a backlog item — by design) is formally acknowledged as
**not a limitation for Tasking** once this feature ships.

---

## Relationship to Sigma.OS Ring 0

In the current user-space implementation, "trusted" means:
- A dedicated R-slot in SYS0's register file.
- A protected slab invisible to normal alloc paths.
- A control page that Memory initializes but the subsystem partially owns.

In the eventual Sigma.OS Ring 0 implementation, "trusted" will mean:
- The slab and control page become permanent entries in **SYS0-DAT** (the bootstrap
  allocation region, offsets 1536–8191), allocated during Phase 1 initialization.
- Trusted subsystem headers are statically positioned in SYS0 alongside the scope table,
  just as Scope 0 (SYS0) and Scope 1 (SLB0) are currently fixed.
- Larger initial slab reservations are negotiated at kernel boot time, not at runtime mmap.
- The `register_sys` call becomes a kernel-mode bootstrap fixture rather than a user-space
  call — but the `sc_trusted_header` layout and the `Allocator.Trusted` API remain the same.

This migration path is intentional. The user-space trusted model implemented here carries forward
to Ring 0 with one documented exception: `TRUSTED_GROWTH_CALLBACK` (R-04). In user space, Memory
calls a Ring 1 function pointer directly. At Ring 0 promotion this up-call violates privilege
separation — Ring 0 must not execute code from a less-privileged ring. The field is redesignated
as an **event sink ID**; Memory posts a growth-request event to a Ring 1 inbox rather than
invoking the callback, and the Ring 1 scheduler processes it on next dispatch. All other API
behaviours are unchanged. The `growth_cb` field comment in `sc_trusted_header` documents this
transition explicitly.

---

## Appendix — Design Review

**Reviewer:** GitHub Copilot  
**Date:** March 11, 2026  
**Summary:** The architecture is sound. Two critical issues must be resolved before implementation
begins. Three notable issues should be addressed or explicitly deferred before Phase 6 tests
are written.

> **All five items below are resolved inline in the body of this document.** The appendix is
> retained for historical context. Each item is annotated with its resolution status.

---

### R-01 ✅ Resolved Inline — Slab Move Hazard (was ❌ Critical)

**Location:** *Growth Policy* section; *mremap and slab_base Stability* section.

**Problem:**  
The plan states: *"Tasking never holds a raw pointer into the slab header — arena base pointers
come from `alloc_arena` return values and are recorded per-fiber. There is no stale-pointer
hazard."*

This is incorrect. `alloc_arena` returns an absolute virtual address. Tasking stores that address
per-fiber (e.g. `f->arena_base`). When `mremap(MREMAP_MAYMOVE)` relocates the slab to a new
virtual address range, every suspended fiber in the scheduler holds a pointer into the
**now-unmapped** old range. Memory updates `header->slab_base` and the SLB0 B-tree node, but
has no mechanism to notify Tasking of the individual per-fiber addresses that have gone stale.

In cooperative mode, growth only occurs during `alloc_arena` (no fiber is executing at that
moment), so there is no immediate crash on growth — but a context switch back to any suspended
fiber after growth would target the old, unmapped stack region.

**Fix:**  
Store fiber arenas as **offsets from `slab_base`**, not absolute addresses:

```c
// In fiber_t: store offset, not absolute pointer
usize arena_offset;  // = alloc_arena_result - header->slab_base at spawn time
usize arena_size;
```

The absolute address is computed on demand as `header->slab_base + f->arena_offset`. Since
`header` is the stable control page pointer (never moves), this is always correct after any
slab relocation — with no notification protocol required between Memory and Tasking.

Note: the saved `rsp` in the fiber's CPU context is an absolute address written at last yield.
That value is correct as long as the fiber is suspended (the stack didn't move under it). The
hazard is only in reconstructing the `arena_base` for `free_arena` at fiber exit — which is
exactly where the offset approach fixes it.

---

### R-02 ✅ Resolved Inline — `fiber_t` Lifetime and Location (was ⚠️ Notable)

**Location:** *Fiber Exit / Reclamation* (`fiber_destroy`) code sample.

**Problem:**  
`fiber_destroy` calls `free_arena(f->arena_base, f->arena_size)` and then `tasking_fiber_free(f)`.
If `fiber_t` is allocated at the base of `arena_base` (the natural and space-efficient choice —
put the bookkeeping struct at the bottom of the stack allocation), then `f` is a dangling pointer
after `free_arena` returns. `tasking_fiber_free(f)` is a use-after-free.

**Fix options (pick one, document the decision):**

1. **Struct in scratch space:** Allocate `fiber_t` entries from the control page scratch region
   (`TRUSTED_SCRATCH_OFFSET` onwards). The scratch space is ~7.6 KB — enough for a fixed
   fiber registry of reasonable size. The struct is never inside the arena.
2. **Struct at arena base, copy-before-free:** Copy the fields needed for `free_arena`
   (`arena_base`/`arena_offset`, `arena_size`) to stack locals before calling `free_arena`,
   then the call to `tasking_fiber_free` is a no-op (nothing to free — the struct was in the
   arena).
3. **Separate small allocation:** Allocate `fiber_t` from a dedicated small-object slab or
   a bump region in scratch space. Cleanest ownership but adds a second allocation path.

Option 1 is consistent with the control page design intent and requires no additional memory
machinery.

---

### R-03 ✅ Resolved Inline — Free-list Overflow (was ⚠️ Notable)

**Location:** *Free-list Node Layout* section.

**Problem:**  
The plan acknowledges the 16-node limit but defers the overflow path to "a second free-list page
at the cost of one additional mmap" without specifying when it triggers, how it is managed, or
what the failure mode is if neither path is taken. The test plan has no coverage for this
boundary.

With highly irregular arena sizes (Sigma.Tasking does support custom `stack_size`), fragmentation
can produce more than 16 distinct free runs. The plan as written has undefined behaviour at
node 17.

**Fix — choose one of the following:**

- **Hard limit (recommended for v0.3.0):** `free_arena` returns `ERR` if the free-list is full.
  Document this as a known constraint. Add test `TAS-07: free_arena ERR at node capacity`.
  Revisit in v0.4.0 with the two-page design.
- **Overflow page (future-proof but more complex):** Specify the second free-list page design
  now — when to `mmap` it, how `sc_slab_free_node.next_off` chains across pages, how
  `free_arena` and `alloc_arena` traverse the chain. Add test `TAS-07: free-list overflow page`.

---

### R-04 ✅ Resolved Inline — `growth_cb` Ring 0 Privilege Model (was ⚠️ Notable)

**Location:** *Growth Policy* table; `sc_trusted_header.growth_cb` field.

**Problem:**  
`TRUSTED_GROWTH_CALLBACK` stores a Ring 1 (Tasking) function pointer in the Ring 0 (Memory)
control page and calls it from inside `trusted_alloc_arena_impl`. In user space this is benign.
At Ring 0 promotion, a kernel-mode Memory component calling up into a Ring 1 function pointer
violates privilege separation — Ring 0 must not execute code supplied by a less-privileged ring.

Additionally, the plan claims the `Allocator.Trusted` API will work unchanged in the Ring 0
build. That claim is false for the CALLBACK policy as currently specified.

**Fix:**  
Document this as a **known future break** in the Ring 0 section. At Ring 0 promotion,
`TRUSTED_GROWTH_CALLBACK` becomes an event/notification posted to a Ring 1 inbox — Memory
blocks the allocation, posts the event, and the Ring 1 scheduler processes it on its next
dispatch. The `growth_cb` function pointer in `sc_trusted_header` is replaced by an event ID
or a ring-buffer slot index. The user-space implementation can remain as-is for v0.3.0.

Add a note to `sc_trusted_header` in the API spec:
```c
// NOTE: growth_cb is valid in user-space only.
// At Ring 0 promotion, this field becomes an event sink ID; the callback model
// is replaced by a Ring 1 inbox notification. TRUSTED_GROWTH_CALLBACK behaviour
// changes accordingly. See Sigma.OS Ring 0 migration guide.
int (*growth_cb)(object sys_page, usize current_size, usize needed);  // 8
```

---

### R-05 ✅ Resolved Inline — Integration Tests Scoped to Memory Simulation (was ⚠️ Notable)

**Location:** *Phase 6 — Tests*, `test_trusted_tasking_sim.c`.

**Problem:**  
`TSM-01..04` are described as "100 fibers spawn+reclaim" with "all policies, valgrind clean".
Actual fiber execution — spawn a fiber, run it, yield, reclaim — requires the assembly
context-switch primitives (`ctx_x86_64_sysv.S` etc.) that are part of Sigma.Tasking's platform
layer. That layer does not exist yet and is not a deliverable of Sigma.Memory v0.3.0.

As written, these tests cannot be implemented without a dependency on an incomplete external
project, which would block the v0.3.0 release.

**Fix:**  
Scope `TSM-01..04` to **memory lifecycle only** for v0.3.0:
- Simulate fibers as plain structs with arena pointers — no actual context switching.
- Test: allocate 100 arenas of mixed sizes, write sentinel values, free in random order,
  verify coalescing, run under valgrind. No `fiber_spawn`, no `task_yield`.
- Label the tests `TSM-01..04 (memory simulation — no fiber execution)`.
- Add a note: *"Full fiber execution integration tests are Phase 4 of the Sigma.Tasking
  implementation plan, gated on the assembly context-switch layer."*
