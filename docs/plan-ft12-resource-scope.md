# Sigma.Memory — FT-12: Resource Scope (`SCOPE_POLICY_RESOURCE`)

**Date:** March 14, 2026
**Feature:** FT-12
**Target Version:** 0.2.4 (or next patch milestone)
**Theme:** Explicit-Lifetime Slab Scopes — No MTIS Overhead, No R7 Coupling
**Methodology:** TDD — Non-Negotiable

---

## Strategic Context

BUG-001 (`docs/bug reports/BUG-001-arena-stale-pointer.md`) revealed that the Anvil team's
core usage pattern — a scoped arena tied to a context object, with an independent lifetime from
R7 — has no clean expression in the current API. The existing `Arena.create` / `Arena.dispose`
model auto-pushes R7, creating an implicit LIFO ordering constraint that does not match Anvil's
natural context lifecycle.

FT-12 introduces `SCOPE_POLICY_RESOURCE`: a new scope kind backed by a single `mmap`'d slab,
with explicit acquire/release lifecycle, pure bump allocation, and zero MTIS overhead. Resource
scopes are **never** set as R7. They are accessed exclusively via the `Allocator.Resource`
sub-interface.

---

## Design

### New typedef: `rscope`

A `rscope` is a pointer to `sc_rscope`, which is **layout-compatible** with `sc_scope` in its
first 16 bytes (the common prefix: `scope_id`, `policy`, `flags`, `_pad`). This allows safe
casting between the two types when dispatch requires it. The rest of the struct is semantically
distinct.

```c
typedef struct sc_rscope *rscope;
```

### `sc_rscope` — 96 bytes, matches `SCOPE_ENTRY_SIZE`

```c
typedef struct sc_rscope {
    usize scope_id;       // 8: Unique ID (matches index in scope_table)
    sbyte policy;         // 1: Always SCOPE_POLICY_RESOURCE (immutable)
    sbyte flags;          // 1: SCOPE_FLAG_* bitmask (mutable)
    sbyte _pad[6];        // 6: Alignment padding

    addr  slab_base;      // 8: Base address of mmap'd slab
    addr  bump_pos;       // 8: Current allocation pointer (slab_base ≤ bump_pos ≤ slab_base + slab_capacity)
    usize slab_capacity;  // 8: Total slab size in bytes (set at acquire, immutable)
    char  name[16];       // 16: Inline scope name (null-terminated)

    addr  nodepool_base;  // 8: Always ADDR_EMPTY — no NodePool for resource scopes

    // Frame support — cursor-save/restore only (no chunk nodes, no B-tree)
    uint16_t current_frame_idx;   // Unused (NODE_NULL) — no NodePool
    uint16_t current_chunk_idx;   // Unused (NODE_NULL) — no NodePool
    uint16_t frame_counter;       // Monotonic frame ID (incremented on each begin)
    bool     frame_active;        // True when a frame is open on this scope
    uint8_t  _frame_pad;          // Alignment padding
    scope    prev;                 // Always NULL — resource scopes never enter R7 chain
    sc_frame_state active_frame;  // Saved cursor state (total_allocated = bump offset at begin)
} sc_rscope;  // Total: 96 bytes
_Static_assert(sizeof(sc_rscope) == SCOPE_ENTRY_SIZE,
               "sc_rscope must match SCOPE_ENTRY_SIZE — update the define");
```

**Invariant:** `bump_pos - slab_base + remaining == slab_capacity` is maintained at all times.
Assert this in debug builds on entry to `resource_alloc` and `resource_reset`.

**Note:** `nodepool_base == ADDR_EMPTY` is the hard sentinel. Any dispatch path that checks
`nodepool_base` before dereferencing the NodePool will correctly no-op or reject a resource scope.

### New policy constant

```c
// In the SCOPE_POLICY enum (include/memory.h):
SCOPE_POLICY_RESOURCE = 3,  // Single mmap slab, bump-only, no MTIS, no R7 coupling
```

### `Allocator.Resource` sub-interface

```c
// New sub-interface (include/memory.h):
typedef struct sc_resource_i {
    rscope  (*acquire)(usize size);               // mmap slab, claim scope_table slot
    object  (*alloc)(rscope s, usize size);       // bump allocate; ALIGN_UP(size, 16)
    void    (*reset)(rscope s, bool zero);        // reset bump cursor; optionally memset
    void    (*release)(rscope s);                 // munmap slab, free scope_table slot
    frame   (*frame_begin)(rscope s);             // save bump cursor as frame marker
    integer (*frame_end)(rscope s, frame f);      // restore bump cursor from frame marker
} sc_resource_i;

// Added to sc_allocator_i:
sc_resource_i Resource;
```

### `Scope.set` guard

`memory_set_current_scope` gains a policy check at entry:

```c
if (s->policy == SCOPE_POLICY_RESOURCE) return ERR;
```

Resource scopes must never enter the R7 activation chain. `prev` is always `NULL`.

---

## Allocation Mechanics

### `resource_acquire(usize size)`

1. Find free slot in `scope_table[2–15]` (same scan as `arena_create_impl`).
2. `mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)`.
3. If `mmap` fails → return `NULL`.
4. Cast the slot to `sc_rscope *`:
   - `scope_id` = slot index
   - `policy` = `SCOPE_POLICY_RESOURCE`
   - `flags` = 0
   - `slab_base` = mmap result
   - `bump_pos` = `slab_base`
   - `slab_capacity` = `size`
   - `nodepool_base` = `ADDR_EMPTY`
   - `prev` = `NULL`
   - `frame_active` = false
5. R7 is **not touched**.
6. Return `(rscope)slot`.

### `resource_alloc(rscope s, usize size)`

```c
usize aligned = ALIGN_UP(size, kAlign);
usize used    = (usize)(s->bump_pos - s->slab_base);
if (used + aligned > s->slab_capacity) return NULL;  // exhausted

object ptr  = (object)s->bump_pos;
s->bump_pos += aligned;
return ptr;
```

No NodePool lookup. No skip list. No B-tree. The hot path is three arithmetic operations and a
bounds check.

### `resource_reset(rscope s, bool zero)`

```c
if (zero) memset((void *)s->slab_base, 0, s->slab_capacity);
s->bump_pos = s->slab_base;
```

`zero == false` → O(1), cursor-only reset. Stale data remains in the slab.
`zero == true`  → O(n), full memset then cursor reset. Clean slate for reuse.

Caller chooses explicitly at the call site. No hidden cost.

### `resource_release(rscope s)`

```c
munmap((void *)s->slab_base, s->slab_capacity);
memset(s, 0, sizeof(sc_rscope));   // clears scope_table slot
```

Slot is eligible for reuse by the next `acquire` or `arena_create` call.
As with `Arena.dispose`: the `rscope` pointer is dangling after this call. Callers must null it.

---

## Frame Support

Resource frames are cursor save/restore — no chunk nodes, no B-tree involvement.

### `resource_frame_begin(rscope s)`

1. Guard: `frame_active == true` → return `NULL` (single active frame per scope, same as arenas).
2. `++s->frame_counter`
3. Save `s->bump_pos - s->slab_base` into `s->active_frame.total_allocated` (uint32_t,
   handles slabs up to 4 GB).
4. `s->active_frame.frame_id = s->frame_counter`
5. `s->frame_active = true`
6. Return packed handle: `(frame)(((uintptr_t)s->scope_id << 16) | s->frame_counter)`

### `resource_frame_end(rscope s, frame f)`

1. Validate handle: upper 16 bits must equal `s->scope_id`, lower 16 bits must equal
   `s->active_frame.frame_id`. Return `ERR` on mismatch.
2. Restore: `s->bump_pos = s->slab_base + s->active_frame.total_allocated`
3. `s->frame_active = false`
4. Return `OK`

All allocations since the matching `frame_begin` are reclaimed in O(1). Memory is not zeroed
(same contract as arena frames). If zeroing on frame-end is needed, caller calls
`resource_reset(s, true)` instead.

---

## `shutdown_memory_system` — leak prevention

The existing shutdown walks `scope_table[2–15]` and calls `nodepool_shutdown`, which no-ops when
`nodepool_base == ADDR_EMPTY`. Resource scopes have `nodepool_base == ADDR_EMPTY`, so an
unreleased resource scope is silently leaked today (once implemented).

Add a policy check in the shutdown walk:

```c
for (usize i = 2; i < 16; i++) {
    scope s = get_scope_table_entry(i);
    if (s == NULL || s->scope_id == 0) continue;

    if (s->policy == SCOPE_POLICY_RESOURCE) {
        rscope rs = (rscope)s;
        if (rs->slab_base != ADDR_EMPTY)
            munmap((void *)rs->slab_base, rs->slab_capacity);
        memset(s, 0, sizeof(sc_rscope));
    } else if (s->nodepool_base != ADDR_EMPTY) {
        // existing arena shutdown path
    }
}
```

---

## `dispatch` guard in `memory_alloc_for_scope`

`arena_alloc` dereferences the NodePool immediately. A resource scope passed to it would
SIGSEGV. Add a policy guard before the default case:

```c
default:
    if (explicit_scope->policy == SCOPE_POLICY_RESOURCE) {
        // Caller must use Resource.alloc(s, size), not Scope.alloc
        ptr = NULL;
    } else {
        ptr = arena_alloc(explicit_scope, size);
    }
    break;
```

---

## Key Invariants & Rules

| Rule | Enforcement |
|---|---|
| `rscope` is never set as R7 | `memory_set_current_scope` returns `ERR` if `policy == SCOPE_POLICY_RESOURCE` |
| No NodePool | `nodepool_base == ADDR_EMPTY` always; `nodepool_init` never called |
| `prev` is always `NULL` | Set in `resource_acquire`; never modified |
| R7 unchanged by acquire/release | `resource_acquire` and `resource_release` do not touch registers |
| Pointer invalid after release | Caller must null `rscope` variable after `Resource.release` |
| Single active frame per scope | `resource_frame_begin` returns `NULL` if `frame_active == true` |
| Slab growth not supported | `resource_alloc` returns `NULL` on exhaustion; caller must size at acquire |

---

## Files Changed

| File | Change |
|---|---|
| `include/memory.h` | Add `SCOPE_POLICY_RESOURCE`; add `rscope` typedef; add `sc_resource_i`; add `.Resource` to `sc_allocator_i` |
| `include/internal/memory.h` | Add `sc_rscope` struct with `_Static_assert`; add `SCOPE_POLICY_RESOURCE` to internal docs |
| `src/memory.c` | Implement `resource_acquire`, `resource_alloc`, `resource_reset`, `resource_release`, `resource_frame_begin`, `resource_frame_end`; wire `resource_iface`; guard `memory_set_current_scope`; guard `memory_alloc_for_scope`; update `shutdown_memory_system` |

---

## Test Plan (TDD — Red first)

All tests in `test/unit/test_resource_scope.c`.

### RS-01 — Acquire returns non-NULL for valid size
```c
rscope s = Allocator.Resource.acquire(65536);
Assert.isNotNull(s, "acquire(64KB) should succeed");
Allocator.Resource.release(s);
```

### RS-02 — Acquire claims a scope_table slot
```c
rscope s = Allocator.Resource.acquire(65536);
Assert.isTrue(s->scope_id >= 2 && s->scope_id <= 15, "slot in user range");
Assert.isEqual(s->policy, SCOPE_POLICY_RESOURCE, "policy set correctly");
Allocator.Resource.release(s);
```

### RS-03 — Alloc returns aligned pointer within slab
```c
rscope s = Allocator.Resource.acquire(65536);
object p = Allocator.Resource.alloc(s, 100);
Assert.isNotNull(p, "alloc should succeed");
Assert.isTrue((uintptr_t)p >= s->slab_base, "within slab lower bound");
Assert.isTrue((uintptr_t)p < s->slab_base + s->slab_capacity, "within slab upper bound");
Assert.isTrue((uintptr_t)p % kAlign == 0, "16-byte aligned");
Allocator.Resource.release(s);
```

### RS-04 — Alloc returns NULL on exhaustion
```c
rscope s = Allocator.Resource.acquire(64);
object p1 = Allocator.Resource.alloc(s, 32);
object p2 = Allocator.Resource.alloc(s, 32);
object p3 = Allocator.Resource.alloc(s, 16);  // would overflow
Assert.isNotNull(p1, "first alloc ok");
Assert.isNotNull(p2, "second alloc ok");
Assert.isNull(p3, "third alloc must return NULL — exhausted");
Allocator.Resource.release(s);
```

### RS-05 — reset(false) restores bump cursor, slab_base unchanged
```c
rscope s = Allocator.Resource.acquire(1024);
Allocator.Resource.alloc(s, 128);
addr bump_after_alloc = s->bump_pos;
Allocator.Resource.reset(s, false);
Assert.isEqual(s->bump_pos, s->slab_base, "bump reset to base");
Assert.isNotEqual(s->bump_pos, bump_after_alloc, "bump moved");
Allocator.Resource.release(s);
```

### RS-06 — reset(true) zeroes slab and resets cursor
```c
rscope s = Allocator.Resource.acquire(256);
object p = Allocator.Resource.alloc(s, 64);
memset(p, 0xAB, 64);
Allocator.Resource.reset(s, true);
// Verify first 64 bytes are zero after reset
byte *mem = (byte *)s->slab_base;
for (usize i = 0; i < 64; i++)
    Assert.isEqual(mem[i], 0, "byte %zu should be zero after zero-reset", i);
Allocator.Resource.release(s);
```

### RS-07 — R7 unchanged by acquire and release
```c
scope before = Allocator.Scope.current();
rscope s = Allocator.Resource.acquire(4096);
Assert.isEqual(Allocator.Scope.current(), before, "acquire must not change R7");
Allocator.Resource.release(s);
Assert.isEqual(Allocator.Scope.current(), before, "release must not change R7");
```

### RS-08 — Scope.set rejected for resource scope
```c
rscope s = Allocator.Resource.acquire(4096);
integer result = Allocator.Scope.set((scope)s);
Assert.isEqual(result, ERR, "Scope.set on resource scope must return ERR");
Allocator.Resource.release(s);
```

### RS-09 — Frame begin/end restores bump cursor
```c
rscope s = Allocator.Resource.acquire(4096);
Allocator.Resource.alloc(s, 256);
addr cursor_before = s->bump_pos;
frame f = Allocator.Resource.frame_begin(s);
Assert.isNotNull(f, "frame_begin should succeed");
Allocator.Resource.alloc(s, 512);
Assert.isNotEqual(s->bump_pos, cursor_before, "cursor advanced in frame");
Allocator.Resource.frame_end(s, f);
Assert.isEqual(s->bump_pos, cursor_before, "cursor restored after frame_end");
Allocator.Resource.release(s);
```

### RS-10 — slot freed after release; next acquire may reuse it
```c
rscope s1 = Allocator.Resource.acquire(1024);
usize id1 = s1->scope_id;
Allocator.Resource.release(s1);
rscope s2 = Allocator.Resource.acquire(1024);
Assert.isEqual(s2->scope_id, id1, "slot recycled after release");
Allocator.Resource.release(s2);
```

### RS-11 — shutdown reclaims unreleased resource scope (valgrind)
```c
// Acquire but do not release — let shutdown_memory_system clean up.
// Run with --valgrind; expect no leak.
rscope s = Allocator.Resource.acquire(8192);
Allocator.Resource.alloc(s, 100);
// No release — test harness shutdown will invoke shutdown_memory_system
```

---

## Acceptance Criteria

- [ ] All RS-01 through RS-11 pass under `./rtest unit`
- [ ] `./rtest unit --valgrind` clean (no leaks, no invalid reads)
- [ ] R7 is unchanged across the full acquire → alloc → reset → release lifecycle
- [ ] `Scope.set(resource_scope)` returns `ERR`
- [ ] `Scope.alloc(resource_scope, size)` returns `NULL` (not a crash)
- [ ] Unreleased resource scope is cleaned up by `shutdown_memory_system`
- [ ] `sizeof(sc_rscope) == 96` — `_Static_assert` passes
- [ ] Existing unit tests unaffected (no regressions)
