# SigmaCore Memory - Roadmap

**Current Version:** 0.1.0  
**Last Updated:** January 29, 2026

---

## How This Works

1. **Everything is prioritized** - debt, features, ideas all in one list
2. **Top 2-3 items** are the next things we work on
3. **After each release**, re-evaluate and reorder
4. **Nothing is rigid** - priorities shift based on needs
5. **Ideas welcome** - even far-out concepts go on the list

---

## 🎯 Next Up

The top items from the prioritized backlog. These are what we're working on now or next.

| # | Item | Type | Effort | Notes |
|---|------|------|--------|-------|
| 1 | Free block size tracking | TD | Mid | Required for proper free list reuse |
| 2 | Dynamic page growth | F | Mid | SLB0 can't grow past 16 pages |
| 3 | `sys0_dispose` coalescing | TD | Mid | Stretch goal for 0.2.0 |

---

## 📋 Prioritized Backlog

Everything we want to do, roughly ordered by priority. Re-evaluated after each release.

### Active / High Priority

| ID | Item | Type | Effort | Description |
|----|------|------|--------|-------------|
| TD-02 | Free block size tracking | Debt | Mid | Store size in hidden header; enables proper free list reuse |
| F-01 | Dynamic page growth | Feature | Mid | mmap new pages when SLB0 exhausted |
| TD-01 | `sys0_dispose` coalescing | Debt | Mid | Mark FREE, merge adjacent blocks |
| F-02 | User arenas | Feature | Hi | scope_table[2-15] for custom memory pools |
| F-03 | Frame checkpoints | Feature | Mid | Save/restore for transactional rollback |
| F-04 | Transient stack | Feature | Mid | LIFO scope for function-local temps |

### Medium Priority

| ID | Item | Type | Effort | Description |
|----|------|------|--------|-------------|
| F-05 | Object promotion | Feature | Mid | Move object from transient → heap |
| F-06 | Cross-scope move | Feature | Mid | Transfer between arenas (copy fallback) |
| F-07 | Scope introspection | Feature | Lo | Stats API for debugging/monitoring |
| F-08 | Allocation callbacks | Feature | Lo | Hooks for profiling (compile-guarded) |

### Low Priority

| ID | Item | Type | Effort | Description |
|----|------|------|--------|-------------|
| F-09 | `SlotArray.set_at()` | Feature | Lo | Cleaner API in sigma.collections |

---

## 💡 Ideas & Future Concepts

Wild ideas, long-term possibilities, things that might never happen but are worth capturing.

| ID | Idea | Notes |
|----|------|-------|
| I-01 | Handle-based allocation | Return handles instead of pointers; enables compaction |
| I-02 | Generational GC layer | Optional GC on top of manual allocation |
| I-03 | Memory-mapped persistence | Scopes that survive process restart |
| I-04 | WASM target | Compile for WebAssembly (no mmap) |
| I-05 | Thread-local arenas | Per-thread allocation pools (lock-free fast path) |
| I-06 | Compressed pointers | 32-bit handles for 64-bit systems (< 4GB heaps) |
| I-07 | Allocation patterns API | Hint system: "burst", "long-lived", "temporary" |
| I-08 | Memory pressure callbacks | Notify app when approaching limits |
| I-09 | Shared memory scopes | Cross-process arenas via shm |
| I-10 | Debug mode poisoning | Fill freed memory with 0xDEADBEEF patterns |

---

## 🏗️ Design Trade-offs (Accepted)

Intentional decisions, not debt. These are features, not bugs.

| ID | Trade-off | Rationale |
|----|-----------|-----------|
| DT-01 | 16-scope limit | Fixed array avoids dynamic allocation; sufficient for most apps |
| DT-02 | 4KB page size | Matches OS page; balances granularity vs overhead |
| DT-03 | No cross-scope pointers | Simplifies ownership; arenas isolated by design |
| DT-04 | Bump-primary allocation | Fast path for common case; free list is fallback |
| DT-05 | 3-attempt free list search | Bounds worst-case time; accepts some fragmentation |
| DT-06 | No automatic compaction | Predictable performance; explicit promotion instead |

---

## 🐛 Known Issues

| ID | Issue | Severity | Notes |
|----|-------|----------|-------|
| KI-01 | Max single alloc ~4032 bytes | Low | Page size minus sentinel; documented limit |
| KI-02 | Stale object files cause test failures | Low | `cbuild clean` when switching test modes |

---

## 📦 Releases

### v0.1.0 ✅ (January 2026)

**Theme:** Bootstrap + User Allocator

**Delivered:**
- SYS0 bootstrap (4KB static page)
- Registers R0-R7 with R7 scope caching
- scope_table[16] unified design
- slab_slots[16] parallel tracking
- SLB0 user allocator (16 pages, 64KB)
- Hybrid bump + free list allocation
- Page release on empty
- Proper shutdown cleanup
- 30 tests (10 bootstrap + 20 slab0)
- Documentation: USERS_GUIDE, MEMORY_REFERENCE, README

---

### v0.2.0 (Next)

**Theme:** Robustness & Growth

**Planned:**
- [ ] TD-02: Free block size tracking
- [ ] F-01: Dynamic page growth
- [ ] TD-01: `sys0_dispose` coalescing (stretch)

---

### Future Releases

Versions beyond 0.2.0 will be scoped based on backlog priorities at the time. No rigid pre-assignment.

---

## Item Details

Detailed descriptions for complex items. Reference by ID.

---

### TD-01: `sys0_dispose` Coalescing

**Problem:** SYS0 uses reclaiming policy but `sys0_dispose()` is a no-op.

**Current:**
```c
static void sys0_dispose(object ptr) {
    (void)ptr;  // TODO: implement
}
```

**Solution:**
1. Mark block as FREE
2. Check next block - if FREE, merge
3. Check previous block (via footer) - if FREE, merge
4. Update footer size

**Risk:** Low - SYS0 allocations are long-lived (scope_table, slotarray).

---

### TD-02: Free Block Size Tracking

**Problem:** `slb0_dispose()` stores `SLB0_MIN_ALLOC` for all freed blocks.

**Current:**
```c
fb->size = SLB0_MIN_ALLOC;  // Conservative estimate
```

**Solution:** Hidden 8-byte header per allocation:
```c
typedef struct {
    usize size;
} sc_alloc_header;
// Payload follows
```

**Impact:** +8 bytes overhead per allocation. Enables proper free list reuse.

---

### F-01: Dynamic Page Growth

**Problem:** SLB0 has DYNAMIC policy but can't grow past 16 pages.

**Solution:**
1. When all pages exhausted, `mmap` new 4KB page
2. Initialize page sentinel
3. Link to end of chain
4. Update scope page_count

**Consideration:** Add max page limit to prevent runaway growth.

---

### F-02: User Arenas

**Purpose:** Custom memory pools in scope_table[2-15].

**API:**
```c
scope arena = Allocator.Arena.create("MyArena", SCOPE_POLICY_FIXED, 4);
object ptr = Allocator.Scope.alloc(arena, 256);
Allocator.Arena.dispose(arena);  // Bulk free
```

---

### F-03: Frame Checkpoints

**Purpose:** Transactional save/restore.

**API:**
```c
frame_id checkpoint = Allocator.Frame.save();
// ... allocations ...
if (error) {
    Allocator.Frame.restore(checkpoint);
} else {
    Allocator.Frame.commit(checkpoint);
}
```

---

### F-04: Transient Stack

**Purpose:** LIFO scope for temporaries, auto-released on exit.

**API:**
```c
Allocator.Transient.enter();
object temp = Allocator.alloc(128);
// ... use temp ...
Allocator.Transient.exit();  // temp invalid
```

**Characteristics:** Bump-only, nestable, zero-cost exit.

---

### F-05: Object Promotion

**Purpose:** Move object from short-lived → long-lived scope.

**API:**
```c
Allocator.Transient.enter();
object temp = Allocator.alloc(64);
object permanent = Allocator.promote(temp);
Allocator.Transient.exit();
// permanent survives
```

**Note:** Caller must update all references.

---

### F-06: Cross-Scope Move

**Purpose:** Transfer between arenas.

**API:**
```c
object moved = Allocator.move(ptr, target_arena);
```

**Implementation:** Copy + free (page transfer only if sole occupant).

---

### F-07: Scope Introspection

**Purpose:** Query scope stats.

**API:**
```c
sc_scope_stats stats;
Allocator.Scope.stats(scope, &stats);
// stats.page_count, .used_bytes, .alloc_count, etc.
```

---

### F-08: Allocation Callbacks

**Purpose:** Hooks for profiling.

**API:**
```c
Allocator.Hook.on_alloc(my_logger);
Allocator.Hook.on_dispose(my_logger);
```

**Note:** Compile-guarded (`#ifdef SIGMA_MEMORY_HOOKS`).

---

## Notes

### Adding Items

- `TD-XX` for technical debt
- `F-XX` for features  
- `I-XX` for ideas
- `DT-XX` for design trade-offs
- `KI-XX` for known issues

### Updating Priorities

After each release:
1. Review what shipped
2. Evaluate remaining items
3. Consider new inputs (bugs, user feedback, ideas)
4. Reorder the backlog
5. Pick next 2-3 items
