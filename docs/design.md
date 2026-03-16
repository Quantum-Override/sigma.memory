# Sigma.Memory v0.3.0 — Controller Model Design

**Date:** March 16, 2026  
**Target Version:** 0.3.0  
**Theme:** Deterministic Allocation via Typed Controllers  
**Status:** Design locked — implementation in TDD phases 0–6  
**Methodology:** TDD — Non-Negotiable

---

## Table of Contents

1. [Context: Why We're Rewriting](#1-context-why-were-rewriting)
2. [Design Principles](#2-design-principles)
3. [Memory Layout Overview](#3-memory-layout-overview)
4. [Core Types](#4-core-types)
   - 4.1 [slab — Raw Memory Region](#41-slab--raw-memory-region)
   - 4.2 [sc_ctrl_base_s — Common Controller Header](#42-sc_ctrl_base_s--common-controller-header)
   - 4.3 [sc_bump_ctrl_s / bump_allocator](#43-sc_bump_ctrl_s--bump_allocator)
   - 4.4 [sc_reclaim_ctrl_s / reclaim_allocator](#44-sc_reclaim_ctrl_s--reclaim_allocator)
5. [Controller Registry](#5-controller-registry)
   - 5.1 [Registry Layout in SYS0](#51-registry-layout-in-sys0)
   - 5.2 [SLB0 as the Registry Allocator](#52-slb0-as-the-registry-allocator)
6. [Public API](#6-public-api)
   - 6.1 [Allocator Facade](#61-allocator-facade)
   - 6.2 [Usage Pattern](#62-usage-pattern)
7. [Frame Semantics per Controller](#7-frame-semantics-per-controller)
   - 7.1 [BumpController Frames](#71-bumpcontroller-frames)
   - 7.2 [ReclaimController Frames](#72-reclaimcontroller-frames)
8. [R7: Fixed to SLB0](#8-r7-fixed-to-slb0)
9. [SYS0 Bootstrap (Unchanged Internals)](#9-sys0-bootstrap-unchanged-internals)
10. [Interface Relocation — sc_allocator_i to sigma.core](#10-interface-relocation--sc_allocator_i-to-sigmacore)
11. [sigma.text Decoupling](#11-sigmatext-decoupling)
12. [Migration from v0.2.x](#12-migration-from-v02x)
13. [What's Removed](#13-whats-removed)
14. [Implementation Phase Plan](#14-implementation-phase-plan)

---

## 1. Context: Why We're Rewriting

v0.2.x accumulated three structural problems that require a ground-up fix:

### 1.1 — String Disposal Non-Determinism (R7 Mismatch)

`Allocator.alloc()` dispatches to the scope pointed to by R7 **at call time**.  
`Allocator.dispose()` also dispatches to R7 **at call time**.

If any code between the alloc and the dispose changes R7 — a `Scope.set()`, an `Arena.create()`,
a frame operation — the disposal goes to the *current* scope, not the *allocating* scope. The
wrong slab frees the pointer. This is silent corruption.

```
// BUG: alloc from SLB0, then Arena.create() pushes R7 → user arena, then dispose goes to arena
// (which never owned the pointer — undefined behaviour)
void *s  = Allocator.alloc(64);        // R7 = SLB0 — correct
scope a  = Arena.create("tmp", BUMP);  // R7 = a    — pushed
Allocator.dispose(s);                  // R7 = a    — WRONG: freed into arena, not SLB0
```

**Fix:** R7 is **permanently fixed to SLB0**. It can never change. Allocation and disposal are
always symmetric without any coordination requirement. See section 8.

### 1.2 — Over-Engineered Defaults

Everything in v0.2.x routed through MTIS (skip-list PageList + B-tree NodePool) — even short-lived
bump-style allocations that would never be individually freed. `Allocator.Resource` (FT-12) was the
right pattern (plain mmap slab, pure bump, reset when done) but it was bolted on as a secondary
API rather than the first-class interface.

The consequence: Anvil's parse allocations paying MTIS overhead for allocations that live only
as long as a parse context. Fixed when Anvil switched to `Allocator.Resource` — but the fix
revealed that `Resource` should have been the default bump story all along.

**Fix:** Explicit typed controllers. The caller declares intent at construction time:
- `create_bump(slab)` → pure cursor, no MTIS, O(1) alloc, O(1) reset
- `create_reclaim(slab)` → MTIS-backed, individual free, split/coalesce

### 1.3 — sc_allocator_i Defined in the Wrong Library

`sc_allocator_i` is the interface type for *any* allocator provider — including `sigma.core.alloc`
(the malloc-backed provider used without sigma.memory). Defining it in `sigma.memory/include/memory.h`
means that any library consuming only `sigma.core.alloc` must pull in `sigma.memory` at the header
level.

sigma.text is the concrete case: its `config.sh` declares `REQUIRES=("sigma.core.alloc")` only,
but its source includes `<sigma.core/memory.h>` which is satisfied at install time by what was
actually the sigma.memory header. The independence was illusory.

**Fix:** `sc_allocator_i` (and the new controller base types) move to `sigma.core`. See section 10.

---

## 2. Design Principles

1. **Separation of memory from policy** — a `slab` is raw memory; a controller is strategy+state.
   One does not imply the other.
2. **Typed vtables over generic nullable function pointers** — `bump_allocator` has `reset`, not
   `free`. `reclaim_allocator` has `free`, not `reset`. No runtime nil checks, no silent no-ops.
3. **Controller struct in SLB0** — controller structs are heap objects managed by the existing
   reclaiming allocator. No separate bump pool, no SYS0 space consumed. SLB0 handles reuse on
   `Allocator.release()`.
4. **Registry in SYS0** — the pointer table (`sc_ctrl_registry_s`) is a small static fixture in
   the SYS0 bootstrap page. It knows about controllers but does not own their memory.
5. **R7 is immutable** — permanently points to SLB0; see section 8.
6. **TDD, no exceptions** — every struct and every function gets a test before implementation.

---

## 3. Memory Layout Overview

```
┌──────────────────────────────────────────────────────────────┐
│  SYS0  (8 KB mmap, static bootstrap page)                    │
│  ├─ R0–R7 virtual register file  (8 × 8 bytes = 64 bytes)    │
│  │    R7 = SLB0 base (fixed, never changes)                  │
│  ├─ scope_table[16]              (16 × 8 bytes = 128 bytes)  │  ← repurposed / stripped in 0.3.0
│  ├─ sc_ctrl_registry_s           (≤ 260 bytes)               │  ← NEW
│  │    entries[32] = pointers to controller structs in SLB0   │
│  │    count (uint8_t)                                        │
│  └─ (SYS0-DAT region: reserved for future OS-level fixtures) │
├──────────────────────────────────────────────────────────────┤
│  SLB0  (dynamic reclaiming slab — mmap, grows on demand)     │
│  ├─ sc_bump_ctrl_s   structs   ← allocated here by create_bump()
│  ├─ sc_reclaim_ctrl_s structs  ← allocated here by create_reclaim()
│  └─ general-purpose allocations via Allocator.alloc/free     │
├──────────────────────────────────────────────────────────────┤
│  User Slab(s)  (acquired via Allocator.acquire(size))        │
│  └─ raw mmap pages, owned by a controller                    │
└──────────────────────────────────────────────────────────────┘
```

Key changes from v0.2.x:
- `scope_table` either shrinks to tracking only SYS0+SLB0 or is removed entirely (slots 2–15 gone)
- `sc_ctrl_registry_s` replaces the 14-slot user arena table
- Controller structs now live **in SLB0**, not in a separate bump pool

---

## 4. Core Types

> All types declared in `sigma.core/include/sigma.core/allocator.h` (see section 10).  
> sigma.memory implements SLB0, SYS0, bootstrap, and the `bump_ctrl` / `reclaim_ctrl` structs.

### 4.1 slab — Raw Memory Region

```c
typedef struct sc_slab_s {
    void    *base;       // mmap base address
    usize    size;       // total mapped bytes
    uint8_t  slab_id;    // unique id (index into SYS0 slab table, if tracked)
} sc_slab_s;
typedef sc_slab_s *slab;
```

A `slab` carries no policy. It is memory and nothing else. Acquired via `Allocator.acquire()`,
released via `Allocator.release()` (which first shuts down the controller that owns it).

### 4.2 sc_ctrl_base_s — Common Controller Header

Every controller struct begins with `sc_ctrl_base_s` at offset 0 so that any controller pointer
can be safely cast to `sc_ctrl_base_s *` for generic operations (release, registry walk).

```c
typedef struct sc_ctrl_base_s {
    sc_alloc_policy  policy;       // POLICY_BUMP or POLICY_RECLAIM
    slab             backing;      // the slab this controller drives
    usize            struct_size;  // sizeof(sc_bump_ctrl_s) or sizeof(sc_reclaim_ctrl_s)
    void (*shutdown)(struct sc_ctrl_base_s *ctrl);  // called by Allocator.release()
} sc_ctrl_base_s;

typedef enum {
    POLICY_BUMP    = 1,  // pure cursor, no individual free
    POLICY_RECLAIM = 2,  // MTIS-backed, individual free
} sc_alloc_policy;
```

### 4.3 sc_bump_ctrl_s / bump_allocator

Pure bump allocator. O(1) alloc. No individual free. `reset` wipes the cursor to 0 for full slab
reuse. `frame_begin/frame_end` save and restore cursor positions for scoped rollback.

```c
#define SC_FRAME_DEPTH_MAX 16

typedef struct sc_bump_ctrl_s {
    sc_ctrl_base_s   base;                          // MUST be first field

    usize            cursor;                        // next-free byte offset
    usize            capacity;                      // usable bytes (slab->size, aligned)

    usize            frame_stack[SC_FRAME_DEPTH_MAX];  // saved cursor positions
    uint8_t          frame_depth;                   // current frame nesting depth

    // vtable
    object (*alloc)      (struct sc_bump_ctrl_s *c, usize size);
    void   (*reset)      (struct sc_bump_ctrl_s *c, bool zero);
    frame  (*frame_begin)(struct sc_bump_ctrl_s *c);
    void   (*frame_end)  (struct sc_bump_ctrl_s *c, frame f);
} sc_bump_ctrl_s;

typedef sc_bump_ctrl_s *bump_allocator;
```

**Behaviour contract:**
- `alloc(c, size)` → ALIGN_UP(cursor, kAlign) + size; returns NULL if cursor + size > capacity
- `reset(c, false)` → cursor = 0 (slab reusable; no munmap)
- `reset(c, true)` → cursor = 0 + memset(base, 0, capacity) (zero-fill, O(n))
- `frame_begin(c)` → push cursor onto frame_stack; returns opaque frame marker
- `frame_end(c, f)` → pop cursor from frame_stack; all allocs since frame_begin are logically gone
- No `free` function — frame rollback is the only reclaim mechanism
- Overflow: frame_begin at depth 16 returns NULL; alloc past capacity returns NULL

### 4.4 sc_reclaim_ctrl_s / reclaim_allocator

MTIS-backed allocator. Individual free, split, coalesce. Same skip-list PageList and B-tree
NodePool internals that powered SLB0 in v0.2.x, now wrapped in a controller struct.
`frame_begin/frame_end` use sequence-tag sweep: tag all allocs after frame_begin with a
monotonic sequence number; frame_end walks and frees all allocs at that tag.

```c
typedef struct sc_reclaim_ctrl_s {
    sc_ctrl_base_s   base;                          // MUST be first field

    void            *nodepool;                      // B-tree NodePool (internal)
    void            *pagelist;                      // skip-list PageList (internal)

    uint32_t         frame_seq;                     // monotonic sequence counter
    uint32_t         active_frame_seq;              // sequence at last frame_begin (0 = none)

    // vtable
    object (*alloc)      (struct sc_reclaim_ctrl_s *c, usize size);
    void   (*free)       (struct sc_reclaim_ctrl_s *c, object ptr);
    object (*realloc)    (struct sc_reclaim_ctrl_s *c, object ptr, usize new_size);
    frame  (*frame_begin)(struct sc_reclaim_ctrl_s *c);
    void   (*frame_end)  (struct sc_reclaim_ctrl_s *c, frame f);
} sc_reclaim_ctrl_s;

typedef sc_reclaim_ctrl_s *reclaim_allocator;
```

**Behaviour contract:**
- `alloc(c, size)` → MTIS bump (within tracked page) or new-page alloc; returns aligned pointer
- `free(c, ptr)` → MTIS coalesce/return-to-pool
- `realloc(c, ptr, new_size)` → in-place shrink or alloc+copy+free grow
- `frame_begin(c)` → snapshot sequence counter; returns frame marker encoding seq
- `frame_end(c, f)` → sweep MTIS and free all allocs with seq ≥ f.seq (batch reclaim)
- SLB0 is a `reclaim_allocator` — the SLB0 instance is the one created at bootstrap

---

## 5. Controller Registry

### 5.1 Registry Layout in SYS0

The registry struct is a **static fixture embedded in the SYS0 bootstrap page** alongside the
register file and the (stripped) scope table. It is not heap-allocated; it lives at a fixed
offset within the 8 KB SYS0 mmap.

```c
#define SC_MAX_CONTROLLERS 32

typedef struct sc_ctrl_registry_s {
    sc_ctrl_base_s  *entries[SC_MAX_CONTROLLERS];  // pointers to ctrlstructs in SLB0
    uint8_t          count;                        // active controller count
} sc_ctrl_registry_s;
// Size: 32 × 8 + 1 = 257 bytes — fits easily in SYS0
```

**Slot 0** is always `(sc_ctrl_base_s *)slb0` — the bootstrap reclaim controller. It is
populated at `init_memory_system()` and is never released.

User controllers occupy slots 1–31. On `Allocator.release()`, the entry is NULLed and the slot
becomes available for the next `create_bump` / `create_reclaim`.

### 5.2 SLB0 as the Registry Allocator

When `Allocator.create_bump(s)` is called:

1. `slb0_alloc(sizeof(sc_bump_ctrl_s))` — allocate the controller struct from SLB0
2. Populate `base.policy`, `base.backing = s`, `base.struct_size`, `base.shutdown`
3. Set vtable function pointers (`alloc`, `reset`, `frame_begin`, `frame_end`)
4. Find the next NULL slot in `registry.entries`, store the pointer, increment `registry.count`
5. Return `(bump_allocator)ctrl`

When `Allocator.release((sc_ctrl_base_s *)a)` is called:

1. Call `a->shutdown(a)` — munmap backing slab, free MTIS internals if reclaim ctrl
2. NULL out the `registry.entries[i]` slot, decrement `registry.count`
3. `slb0_dispose(a)` — return controller struct to SLB0; MTIS handles split/coalesce as usual

This means:
- **No separate bump pool** — no `sc_ctrl_registry_s.pool[]` array (was in the previous design sketch)
- **No capacity planning** for controller struct storage — SLB0 grows dynamically as always
- Controller struct reuse is handled by MTIS coalescing, same as any other SLB0 alloc

---

## 6. Public API

### 6.1 Allocator Facade

Defined in `sigma.core/include/sigma.core/allocator.h`; implemented in sigma.memory.

```c
typedef struct sc_allocator_i {
    // Raw slab lifecycle
    slab             (*acquire)(usize size);               // mmap + slab_id assignment
    void             (*release)(sc_ctrl_base_s *ctrl);     // shutdown(ctrl) + slb0_dispose(ctrl) + munmap slab

    // Typed controller constructors (struct allocated in SLB0, registered)
    bump_allocator   (*create_bump)(slab s);
    reclaim_allocator(*create_reclaim)(slab s);

    // Top-level facade — dispatches to SLB0 reclaim ctrl (drop-in compat)
    object           (*alloc)(usize size);
    void             (*free)(object ptr);
    object           (*realloc)(object ptr, usize new_size);
} sc_allocator_i;

extern const sc_allocator_i Allocator;
```

### 6.2 Usage Pattern

```c
// ── Bump controller (e.g. Anvil parse context) ──────────────────────
slab s            = Allocator.acquire(8 * 1024 * 1024);   // 8 MB raw slab
bump_allocator a  = Allocator.create_bump(s);             // ctrl struct in SLB0

void *tokens      = a->alloc(a, token_count * sizeof(Token));
frame f           = a->frame_begin(a);                    // snapshot cursor
void *scratch     = a->alloc(a, 4096);
                    a->frame_end(a, f);                   // scratch gone; tokens intact
                    a->reset(a, false);                   // all gone; slab reusable
Allocator.release((sc_ctrl_base_s *)a);                   // munmap slab; slb0_dispose(ctrl)

// ── Reclaim controller (general-purpose, long-lived) ─────────────────
slab t            = Allocator.acquire(2 * 1024 * 1024);
reclaim_allocator r = Allocator.create_reclaim(t);

void *node        = r->alloc(r, sizeof(AstNode));
                    r->free(r, node);
frame g           = r->frame_begin(r);
void *batch       = r->alloc(r, 65536);
                    r->frame_end(r, g);                   // batch freed by sweep
Allocator.release((sc_ctrl_base_s *)r);

// ── Drop-in SLB0 facade (backward compat; also sigma.core.alloc path) ─
void *p           = Allocator.alloc(256);
                    Allocator.free(p);
void *q           = Allocator.realloc(p2, 512);
```

---

## 7. Frame Semantics per Controller

### 7.1 BumpController Frames

A frame is a **cursor snapshot**. `frame_begin` saves the current cursor; `frame_end` restores it.
Allocations made between begin and end are "freed" by simply moving the cursor back — they are not
individually tracked, not coalesced. Zero overhead: one array write and one counter increment.

```
cursor:  0──────────[frame0]────────────────────[frame1]─────────cursor
                    ↑ saved                                       current
                    │
            frame_stack[0]
```

- Nest up to `SC_FRAME_DEPTH_MAX` (16) frames — sufficient for recursive-descent parsers
- `frame_begin` on a full stack returns NULL; caller must check
- `frame_end` with mismatched frame (wrong depth) is undefined; debug builds assert

### 7.2 ReclaimController Frames

A frame is a **sequence-tag boundary**. Every alloc after `frame_begin(r)` is tagged with the
current monotonic `frame_seq`. `frame_end(r, f)` sweeps the MTIS PageList and frees all allocs
tagged with that sequence number. Individual allocs made *before* the frame are unaffected.

```
alloc(a) seq=0  alloc(b) seq=0  frame_begin→seq=1  alloc(c) seq=1  alloc(d) seq=1
                                                                     ↓ frame_end
                                                    free(c), free(d) — a, b untouched
```

- Once frame_end fires, the sequence reverts to `active_frame_seq - 1`
- Frames on a reclaim controller are more expensive than bump frames (linear sweep of tagged allocs)
- Use bump controllers when you know the allocation pattern is strictly LIFO; reclaim frames for
  mixed patterns where some pre-frame allocs still need individual free

---

## 8. R7: Fixed to SLB0

In v0.2.x, R7 was the "current scope" — a mutable global pointer pushed and popped as arenas
were created and disposed. This was the root cause of the string disposal bug (section 1.1).

**In v0.3.0, R7 is a read-only constant.** It is set once at `init_memory_system()` to the
address of the SLB0 reclaim controller and never written again.

Consequences:
- `Allocator.alloc(size)` always allocates from SLB0. Always. Non-negotiable.
- `Allocator.free(ptr)` always frees to SLB0. Always.
- Scope push/pop (`Scope.set`, `Scope.restore`) is removed. There is no scope stack.
- `Allocator.alloc/free` remain symmetric regardless of what controllers exist or what any other
  code has done in between the alloc and the free call.
- Users wanting arena-style isolation use `bump_allocator` with explicit `a->alloc(a, size)`.
  The explicit `a` argument is the "scope" — determined at the call site, not from a global.

---

## 9. SYS0 Bootstrap (Unchanged Internals)

The SYS0 8 KB bootstrap page and the bootstrap sequence are structurally unchanged from v0.2.x
at the machine level. What changes is the *interpretation* of parts of the page:

| Region | v0.2.x | v0.3.0 |
|--------|--------|--------|
| Offset 0: register file R0–R7 (64 bytes) | R7 = current scope (mutable) | R7 = SLB0 (immutable) |
| Offset 64: scope_table[16] (128 bytes) | 0=SYS0, 1=SLB0, 2–15=user arenas | stripped; only SYS0+SLB0 relevant |
| Offset 192: (new) sc_ctrl_registry_s (≤ 260 bytes) | n/a | controller pointer table |
| Remainder: SYS0-DAT region | node_table, etc. | unchanged |

`init_memory_system()` still uses `__attribute__((constructor(101)))` for priority ordering.
The `-Wno-attributes` suppression in `.vscode/c_cpp_properties.json` is carried over.

---

## 10. Interface Relocation — sc_allocator_i to sigma.core

### Problem

`sigma.core.alloc` (sys_alloc.c) implements the malloc-backed `Allocator`. It currently includes
`<sigma.memory/memory.h>` to get the `sc_allocator_i` type. This is backwards — sigma.core is
lower in the dependency chain than sigma.memory.

sigma.text declares `REQUIRES=("sigma.core.alloc")` and includes `<sigma.core/memory.h>`. That
path is satisfied at install from sigma.memory. Independence is fake.

### Fix

Create `sigma.core/include/sigma.core/allocator.h` with:
- `sc_alloc_policy` enum
- `sc_ctrl_base_s` struct (opaque to sigma.core; defined in full only in sigma.memory)
- `slab` typedef (forward; sigma.core.alloc doesn't need internals)
- `sc_allocator_i` with: `alloc`, `free`, `realloc`, `acquire`, `release`, `create_bump`, `create_reclaim`
- `bump_allocator` and `reclaim_allocator` opaque typedefs (pointers to structs defined in sigma.memory)
- `extern const sc_allocator_i Allocator;`

`sigma.core/src/sys_alloc.c`:
- Switch `#include <sigma.memory/memory.h>` → `#include <sigma.core/allocator.h>`
- `acquire/create_bump/create_reclaim` return NULL or abort in the malloc-backed implementation
  (sigma.core.alloc is a minimal fallback, not a full memory system)

sigma.memory's `include/memory.h`:
- `#include <sigma.core/allocator.h>` at top (gets the interface type)
- Defines `sc_bump_ctrl_s`, `sc_reclaim_ctrl_s`, `sc_ctrl_registry_s` (full definitions)
- No longer `extern const sc_allocator_i Allocator;` (that stays in sigma.core)

sigma.text:
- `REQUIRES=("sigma.core")` only — genuinely independent
- `#include <sigma.core/allocator.h>` for `sc_allocator_i`
- Fix `Allocator.alloc(size, false)` → `Allocator.alloc(size)` (remove spurious bool arg)

---

## 11. sigma.text Decoupling

sigma.text's strings.c uses `Allocator.alloc/free` for all string and StringBuilder allocation.
The current code has `Allocator.alloc(size, false)` — the second bool argument does not exist
in the current `sc_allocator_i`. This is a pre-existing divergence that must be fixed as part
of Phase 0.

After Phase 0:
- `strings.c` calls `Allocator.alloc(size)` — allocates from SLB0 (sigma.memory must be up)
  *or* from the malloc-backed fallback (sigma.core.alloc) if sigma.memory is absent
- `string_dispose(str)` calls `Allocator.free(str)` — always symmetric (R7 fixed = SLB0)
- The disposal bug is gone by construction

`sigma.text/config.sh` changes:
```bash
# before:
REQUIRES=("sigma.core.alloc")

# after:
REQUIRES=("sigma.core")   # gets sc_allocator_i from allocator.h
# (sigma.memory is the runtime provider; sigma.text doesn't link it directly)
```

---

## 12. Migration from v0.2.x

| v0.2.x pattern | v0.3.0 equivalent |
|---|---|
| `Allocator.alloc(size)` | `Allocator.alloc(size)` — unchanged, goes to SLB0 |
| `Allocator.dispose(ptr)` | `Allocator.free(ptr)` — renamed for clarity |
| `Allocator.realloc(ptr, size)` | `Allocator.realloc(ptr, size)` — unchanged |
| `scope s = Allocator.Arena.create("name", BUMP)` | `slab sl = Allocator.acquire(size); bump_allocator a = Allocator.create_bump(sl);` |
| `Allocator.Arena.alloc(size)` → uses R7 | `a->alloc(a, size)` — explicit controller |
| `Allocator.Arena.dispose(s)` | `Allocator.release((sc_ctrl_base_s *)a)` |
| `rscope rs = Allocator.Resource.acquire(size)` | `slab sl = Allocator.acquire(size); bump_allocator a = Allocator.create_bump(sl);` |
| `Allocator.Resource.alloc(rs, size)` | `a->alloc(a, size)` |
| `Allocator.Resource.reset(rs, zero)` | `a->reset(a, zero)` |
| `Allocator.Resource.release(rs)` | `Allocator.release((sc_ctrl_base_s *)a)` |
| `Allocator.Frame.begin()` (on R7 scope) | `a->frame_begin(a)` — explicit controller |
| `Allocator.Frame.end(f)` | `a->frame_end(a, f)` |
| `Allocator.promote(ptr, size, dst)` | removed — copy explicitly or alloc+memcpy |
| `Allocator.Scope.current()` | removed — no mutable scope |
| `Allocator.Scope.set(ptr)` | removed — R7 is fixed |
| `Allocator.Scope.restore()` | removed — no scope stack |

---

## 13. What's Removed

The following are **deleted** in v0.3.0 with no replacement:

| Symbol | Reason |
|---|---|
| `sc_allocator_scope_i` (`Allocator.Scope`) | R7 is fixed; mutable scope concept gone |
| `sc_arena_i` (`Allocator.Arena`) | Replaced by explicit `bump_allocator` or `reclaim_allocator` |
| `sc_resource_i` (`Allocator.Resource`) | Merged into `bump_allocator` (was always bump+mmap) |
| `sc_frame_i` (`Allocator.Frame`) | Per-controller `frame_begin/frame_end` replace the facade |
| `Allocator.promote` | No cross-scope move semantics needed; callers own their allocs |
| `scope_table[2..15]` | 14 user arena slots disappear; replaced by registry pointer table |
| `SCOPE_POLICY_DYNAMIC` | Controllers don't carry policy enum for growth; slab is pre-sized |
| `SCOPE_POLICY_RECLAIMING` | Only ever applied to SYS0/SLB0; unnecessary externally |
| `SCOPE_POLICY_FIXED` | Replaced by `create_bump` with pre-sized slab (same semantics) |
| `SCOPE_POLICY_RESOURCE` | Merged into `create_bump` |
| `SCOPE_FLAG_*` | No user-visible scope objects; flags have no target |
| `frame_depth`, `frame_allocated` globals | Per-controller state only |

---

## 14. Implementation Phase Plan

All phases use TDD: write the test, see it fail, write the implementation, see it pass, Valgrind clean.

### Phase 0 — Interface Relocation + sigma.text Fix
**Goal:** Get `sc_allocator_i` cleanly into sigma.core; fix sigma.text bool-arg divergence.

Tasks:
- [ ] Create `sigma.core/include/sigma.core/allocator.h` with `sc_allocator_i`, opaque `bump_allocator` / `reclaim_allocator` typedefs, `slab` forward typedef
- [ ] Update `sigma.core/src/sys_alloc.c`: `#include <sigma.core/allocator.h>` (remove sigma.memory dep)
- [ ] Update `sigma.core/config.sh` to install `allocator.h`
- [ ] Fix `sigma.text/src/strings.c`: `Allocator.alloc(size, false)` → `Allocator.alloc(size)`
- [ ] Update `sigma.text/config.sh`: confirm `REQUIRES=("sigma.core")`
- [ ] Confirm sigma.text builds clean without sigma.memory in link path

### Phase 1 — slab Type + acquire/release (raw)
**Tests:** SLB-01..04: acquire 4KB, acquire 8MB, double-release assert, slab_id assignment

Tasks:
- [ ] Define `sc_slab_s` in `sigma.memory/include/memory.h`
- [ ] Implement `slb_acquire(size)` — mmap, assign slab_id (from SYS0 counter)
- [ ] Implement `slb_release_raw(slab s)` — munmap only (no controller involved)

### Phase 2 — BumpController
**Tests:** BC-01..12: alloc within capacity, alloc exact capacity, alloc over capacity (NULL),
reset reuse, reset zero-fill, frame_begin/end round-trip, nested frames (depth 4),
frame overflow (depth 17 → NULL), frame_end restore correctness

Tasks:
- [ ] Define `sc_bump_ctrl_s` in `sigma.memory/include/memory.h`
- [ ] Implement `bump_ctrl_alloc`, `bump_ctrl_reset`, `bump_ctrl_frame_begin`, `bump_ctrl_frame_end`
- [ ] Implement `bump_ctrl_shutdown` (munmap slab)

### Phase 3 — ReclaimController
**Tests:** RC-01..14: alloc, free, realloc shrink, realloc grow, realloc NULL, frame_begin/end
basic sweep, frame_end does-not-affect-pre-frame allocs, double free (debug assert), valgrind clean

Tasks:
- [ ] Define `sc_reclaim_ctrl_s` in `sigma.memory/include/memory.h`
- [ ] Port MTIS (NodePool + PageList) from sigma.mem_0.2 into reclaim controller internals
- [ ] Implement sequence-tag tracking on alloc + frame_begin/frame_end sweep
- [ ] Implement `reclaim_ctrl_shutdown` (MTIS teardown + munmap slab)
- [ ] Wire SLB0 bootstrap to use `sc_reclaim_ctrl_s` instance (R7 ← SLB0)

### Phase 4 — Registry + create_bump / create_reclaim
**Tests:** REG-01..08: create_bump / create_reclaim allocates from SLB0, registered in table,
Allocator.release NULLs slot, SLB0 reuse after release, 31 controllers (near-max),
32nd controller (max), 33rd (assert/NULL)

Tasks:
- [ ] Embed `sc_ctrl_registry_s` in SYS0 at bootstrap
- [ ] Implement `allocator_create_bump(slab s)` — slb0_alloc + vtable init + register
- [ ] Implement `allocator_create_reclaim(slab s)` — slb0_alloc + vtable init + register
- [ ] Implement `allocator_release(sc_ctrl_base_s *ctrl)` — shutdown + deregister + slb0_dispose

### Phase 5 — Public Facade + Drop-in Compat
**Tests:** FAC-01..06: Allocator.alloc/free/realloc dispatch correctly to SLB0,
Allocator.acquire returns slab, create_bump round-trip via Allocator, all v0.2.x
callsites that survive migration still build and pass

Tasks:
- [ ] Populate `const sc_allocator_i Allocator` with all function pointers
- [ ] Regression pass: port sigma.mem_0.2 test suites that are still relevant
- [ ] Full Valgrind pass (all test suites, 0 bytes leaked)

### Phase 6 — sigma.text Integration Verification
**Tests:** TXT-01..04: strings.c alloc/dispose round-trip, StringBuilder grow/reset,
sigma.text builds with REQUIRES=("sigma.core") only (no sigma.memory in link), Valgrind clean

Tasks:
- [ ] `sigma.text cbuild` clean
- [ ] `sigma.text rtest unit` all passing
- [ ] Confirm no sigma.memory symbols in sigma.text.o (`nm` check)

---

## Appendix A — Rationale for SLB0 as Registry Allocator

The previous design sketch (from the session ending March 15) proposed a `pool[]` bump array
embedded inside `sc_ctrl_registry_s` in SYS0 for controller struct storage. This was discarded
for three reasons:

1. **SYS0 is 8 KB. Controller structs are non-trivial.**  
   `sc_reclaim_ctrl_s` with NodePool and PageList pointers, vtable pointers, and sequence numbers
   is on the order of 64–80 bytes. 32 controllers × 80 bytes = 2.5 KB — a significant fraction of
   SYS0, crowded out future SYS0-DAT fixtures.

2. **SLB0 already solves this problem.**  
   SLB0 is a dynamic reclaiming allocator that handles variable-size allocs of exactly this scale.
   Controller structs are only allocated at controller creation time (low frequency). SLB0 reuses
   their slots cleanly on release. Zero additional mechanism needed.

3. **Simplicity.**  
   The registry is just `sc_ctrl_base_s *entries[32]` — 257 bytes in SYS0. Everything else goes
   through existing machinery. No bump pool to size, no overflow to handle, no separate cursor.
