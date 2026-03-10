# SigmaCore Memory — Backlog

**Version:** 0.2.3-realloc  
**Last Updated:** March 9, 2026  
**Maintained By:** SigmaCore Development Team

This document tracks all known gaps, deferred features, and suggested improvements. Items are
prioritized by impact and grouped by category. Each entry states what problem it solves and
why it matters.

---

## Priority Key

| Rating | Meaning |
|--------|---------|
| 🔴 **Critical** | Correctness or safety risk; blocks production use |
| 🟠 **High** | Significant usability or reliability gap; near-term milestone |
| 🟡 **Medium** | Meaningful improvement; target v0.3.0 |
| 🟢 **Low** | Nice-to-have; quality-of-life or future-proofing |

---

## Bugs / Correctness Gaps

### BG-01 — `registers_get`/`registers_set`: `-Wtype-limits` warning 🟠 High

**File:** `src/memory.c:92,100`  
**Problem:** `sbyte index` (signed 8-bit) is compared against `index < 0`. Because `sbyte` is
`int8_t`, GCC correctly warns `-Wtype-limits: comparison is always false due to limited range of
data type` when building with `-Wextra` (e.g., via `cbuild lib`). The guard can never fire.

**What it solves:** Eliminates a dead guard, silences the warning, and prevents the function from
silently accepting invalid index values with no bounds enforcement.

**Fix:** Change `sbyte index` to `byte index` (unsigned 8-bit, per `sigma.core/types.h`). Add `assert(index <= 7)` in debug builds.

---

### BG-02 — `SCOPE_POLICY_RECLAIMING` silently degrades for user arenas 🟠 High

**File:** `src/memory.c` / `include/memory.h`  
**Problem:** `SCOPE_POLICY_RECLAIMING = 0` is documented as "first-fit with block reuse (SYS0
only)". When a caller passes it to `Allocator.create_arena()`, the arena silently uses bump
allocation (same as `SCOPE_POLICY_DYNAMIC`) — there is no reclaiming behaviour and no error.
This is a silent semantic mismatch.

**What it solves:** Prevents user confusion when arenas don't free and reuse blocks as they
expect. Either enforce the constraint (return `NULL`/log error when `RECLAIMING` is passed to
`create_arena`) or implement reclaiming arenas.

**Fix:** `create_arena(name, SCOPE_POLICY_RECLAIMING)` should create a `SlabAllocator` scope — B-tree-tracked, split/coalesce, individual dispose — essentially a named SLB0-equivalent. Arenas will **always** remain pure bump; `RECLAIMING` is the signal to create a user slab. See FT-05.

---

### BG-03 — `SCOPE_POLICY_FIXED` enforcement for arenas untested 🟡 Medium

**File:** `src/memory.c` — `arena_alloc_for_scope`  
**Problem:** `SCOPE_POLICY_FIXED` is declared and documented but no test suite verifies it
actually returns `NULL` when the first (and only) page is exhausted. If the bump allocator chains
pages regardless of the policy flag, the fixed constraint is silently violated.

**What it solves:** Prevents silent over-allocation in size-bounded use cases (e.g., plugin
sandboxes with a hard memory cap).

**Fix:** Add a policy guard in the arena allocator path + add test cases `FXD-01..03` (alloc up
to limit returns data, alloc past limit returns NULL, page_count stays at 1).

---

### BG-04 — No coalescing in SYS0's B-tree after `realloc` shrink-split 🟡 Medium

**File:** `src/memory.c` — `slb0_realloc`  
**Problem:** When `realloc` shrinks a block and splits the remainder into a free node, adjacent
free nodes are not automatically coalesced. Sequential shrink → realloc cycles on the same
region can fragment SYS0 into many small free nodes, degrading B-tree search performance toward
O(n).

**What it solves:** Prevents long-term fragmentation accumulation in workloads that repeatedly
resize allocations.

**Fix:** After inserting the split free node, call the existing coalesce path
(`btree_coalesce` / `node_free`) to merge with any adjacent free block.

---

## Missing Features

### FT-01 — Thread safety (Sigma.Tasking integration) 🔴 Critical (for multi-threaded use)

**File:** `include/memory.h:29` — "designed for single-threaded, non-concurrent use"  
**Problem:** All allocation paths share mutable global state (registers, scope table, page
chains) with no synchronisation. Any concurrent access produces data races and undefined
behaviour.

**What it solves:** Enables `sigma.memory` in multi-threaded runtimes (Sigma.Tasking, server
request handlers, parallel test runners).

**Design notes (from ROADMAP):**
- Hook-based model: Sigma.Tasking supplies lock/unlock callbacks; `sigma.memory` calls them
  around critical sections. No direct pthread dependency.
- Thread-local `R7` (current scope) is likely the right model; global SYS0 mutations need a
  global lock.
- Blocked on: Sigma.Tasking API stability.

> ⚠️ **Sigma.Tasking is entering active planning.** Revisit this item as soon as Sigma.Tasking API design begins. Hook signatures must be co-designed with it.

---

### FT-02 — `Allocator.alloc(size, bool zee)` — zero-init parameter 🟠 High

**Problem:** There is no zero-initialising allocation primitive. Every caller that needs zeroed
memory must call `Allocator.alloc` + `memset`, which is error-prone (forgetting `memset`),
verbose, and misses potential OS-provided zero pages.

**What it solves:** Reduces boilerplate; avoids unintentional use of uninitialised memory.

**Design:** This parameter was present in early prototyping. Restore `bool zee` on `alloc`
rather than adding a separate `calloc`. When `zee == true`, zero the allocation before returning.
For fresh mmap'd pages the OS already zeroes them — an optimised path can skip `memset` in that
case.

```c
object buf  = Allocator.alloc(256, false);  // existing behaviour
object zbuf = Allocator.alloc(256, true);   // zeroed on return
```

> ⚠️ **Breaking change** — all existing `Allocator.alloc(size)` call sites must be updated to
> `alloc(size, false)`. Requires a minor version bump and a migration guide.

---

### FT-03 — Scope introspection / stats API 🟠 High

**Problem:** There is no way to query the live state of a scope: how many pages it holds, how
many bytes are allocated, how many free nodes exist, or what the fragmentation ratio is.
Debugging allocation pressure or memory leaks requires attaching a debugger.

**What it solves:** Enables real-time monitoring, test assertions on allocator state, and
profiling without external tools.

**Proposed API:**
```c
typedef struct sc_scope_stats {
    usize page_count;          // Mapped pages
    usize alloc_bytes;         // Live allocated bytes
    usize free_bytes;          // Free bytes tracked in B-tree
    usize free_node_count;     // Free B-tree entries (fragmentation proxy)
    usize largest_free_block;  // Max contiguous free bytes (single alloc limit)
} sc_scope_stats;

integer Allocator.Scope.stats(scope s, sc_scope_stats *out);
```

**Effort:** Low — data already lives on the scope struct; this is an accessor/aggregator, not a new data structure.

---

### ~~FT-04~~ — Arena frame support ✅ Resolved (v0.2.3)

**Resolution:** `Frame.begin_in(scope)` / `Frame.end_in(scope, frame)` and
`Arena.frame_begin(scope)` / `Arena.frame_end(scope, frame)` were implemented in the v0.2.3
sub-interface split. Frames within arenas work without changing R7.

**Follow-up:** Add `test/unit/test_arena_frames.c` to confirm `begin_in` + `end_in` on a
user arena allocates, bulk-disposes correctly, and leaves the arena intact. Tracked as TG-06.

---

### FT-05 — `SlabAllocator` scope via `SCOPE_POLICY_RECLAIMING` 🟡 Medium

**Problem:** There is no way to create a user-defined slab scope (B-tree-tracked, individual
alloc/dispose, split/coalesce) beyond the single built-in SLB0. User arenas are pure bump only.

**What it solves:** Allows subsystems that need full reclaiming allocation semantics to get an
isolated, named slab scope — essentially a user-created SLB0 equivalent.

**Design:** Arenas will **always** be pure bump. `SCOPE_POLICY_RECLAIMING` passed to
`create_arena(name, SCOPE_POLICY_RECLAIMING)` returns a new `SlabAllocator` scope — backed by
skip list + per-page B-trees, not a bump arena. All SLB0 mechanics apply: dynamic page growth,
page release on empty, `realloc`, coalesce.

**Implementation:** Each user scope already has a NodePool (v0.2.2). Wire
`arena_alloc_for_scope` / `arena_dispose_for_scope` through the `slb0_alloc` / `slb0_dispose`
path when `policy == SCOPE_POLICY_RECLAIMING`. Bulk-dispose (`dispose_arena`) still applies.

**See also:** BG-02 — must be fixed before or alongside this feature.

---

### FT-06 — B-tree rebalancing (pathological fragmentation protection) � Low

**File:** `src/node_pool.c`  
**Problem:** The per-page B-tree is an unbalanced BST ordered by block start address. For
pathological allocation patterns (many same-size allocs, free in sorted order), the tree
degenerates to a linked list, turning O(log n) searches into O(n).

**What it solves:** Maintains O(log n) worst-case guarantee regardless of allocation pattern.

**Options:** AVL rotation (lightweight, predictable) or red-black tree (standard).
Alternatively, a skip list at the B-tree level (consistent with the outer skip list design).

**Note:** By design, per-page B-trees stay shallow (≤ ~15 nodes per page). Degenerate cases
are unlikely in practice and may never warrant balancing. Re-evaluate only if profiling under
adversarial workloads shows measurable O(n) behaviour.

---

### FT-07 — `Allocator.Scope.resize(scope, new_policy)` — post-creation policy change 🟢 Low

**Problem:** Scope policy is fixed at creation time and cannot be changed. An arena created with
`SCOPE_POLICY_DYNAMIC` cannot be downgraded to `SCOPE_POLICY_FIXED` after initial pages are
mapped.

**What it solves:** Supports lifecycle transitions — e.g., a pre-loading phase (dynamic) that
locks down to fixed capacity once initialisation is complete, preventing runaway growth.

---

### FT-08 — Memory pressure callbacks / exhaustion hooks 🟡 Medium

**Problem:** When SLB0 or an arena approaches exhaustion, the caller only learns about it via a
`NULL` return from `alloc`. There is no pre-emptive notification to allow graceful degradation
(e.g., flush caches, compact, or reject new work before allocations start failing).

**What it solves:** Enables robust memory management in embedded or constrained environments
where `NULL` from `alloc` may be too late to recover gracefully.

**Proposed API:**
```c
typedef void (*sc_pressure_fn)(scope s, usize free_bytes, void *ctx);
integer Allocator.Scope.on_pressure(scope s, sc_pressure_fn fn, void *ctx, usize threshold);
```

---

### FT-09 — `SCOPE_FLAG_SECURE` zeroing on dispose 🟡 Medium

**File:** `include/memory.h` — `SCOPE_FLAG_SECURE = 0x04` declared but not implemented  
**Problem:** The `SCOPE_FLAG_SECURE` flag is defined in `sc_scope_flags` but `slb0_dispose` and
`arena_dispose_for_scope` do not check it or zero memory on free. Sensitive data (keys, tokens,
credentials) survives in freed blocks and is exposed to subsequent allocations.

**What it solves:** Crypto and security workloads that require memory scrubbing before reuse.

**Implementation:** In `slb0_dispose` / `arena_dispose_for_scope`, check `scope->flags &
SCOPE_FLAG_SECURE`; if set, `memset(ptr, 0, size)` before inserting the free node into the
B-tree.

**Effort:** Low — `memset` + a flag check in two functions.

---

### FT-10 — `Allocator.Scope.compact(scope)` — explicit defragmentation 🟢 Low

**Problem:** There is no way for callers to trigger a B-tree compaction pass. Over time, heavy
alloc/free cycling leaves the B-tree with many small free nodes. The coalesce pass runs
opportunistically on `dispose`, but callers cannot force a full-scope merge.

**What it solves:** Allows batch applications (compilers, parsers) to compact after a heavy
phase before starting the next, recovering contiguous free space for large allocations.

---

## Testing Gaps

### TG-01 — Code coverage tooling not set up 🟠 High

**Problem:** The ROADMAP targets 90%+ line coverage and 85%+ branch coverage, but there is no
`gcov`/`lcov` integration in the build system. Coverage is entirely aspirational with no
measurement.

**What it solves:** Turns a qualitative claim into a tracked metric. Exposes untested branches
(error paths, edge conditions) that may hide bugs like BG-01/BG-02.

**Fix:** Add a `cbuild coverage` target: compile with `-fprofile-arcs -ftest-coverage`, run test
suite, generate `lcov` report to `docs/coverage/`. Integrate into CI.

> **Note:** Coverage tooling will also land via the **Sigma.X** toolchain. The `cbuild coverage`
> target above is a useful local addition in the meantime.

---

### TG-02 — Performance benchmarks not in regular CI 🟡 Medium

**Problem:** `test/performance/test_allocation_throughput.c` and `test_search_scaling.c` exist
but are not run as part of the standard `rtest unit` or `rtest integration` pass. Regressions in
allocation speed or B-tree search time go undetected.

**What it solves:** Catches performance regressions before release; gives baseline numbers for
tuning work (FT-05, FT-06).

**Fix:** Add a `rtest perf` category; run benchmarks in CI in opt mode; record baseline numbers
in `test/performance/benchmark_journal.md` and fail if throughput drops >10%.

> **Note:** Performance tracking tooling will also arrive via the **Sigma.X** toolchain.
> Maintain `benchmark_journal.md` manually in the meantime to preserve baseline data.

---

### TG-03 — Integration test suite partially blocked � High

**File:** `test/integration/test_integration.c`, `docs/PHASE7_TEST_AUDIT.md`  
**Problem:** Several integration tests were marked `⏸️ BLOCKED` during Phase 7 development and
have not been revisited since Phase 7 completed. The test suite may contain stale assertions or
incomplete coverage of the full alloc → grow → dispose lifecycle across scopes.

**What it solves:** Ensures the system-level integration path (SLB0 + NodePool + arenas + frames
interacting together) has the same test confidence as the unit layer.

**Fix:** Audit `test/integration/test_integration.c` against current API; update or add test
cases for the full lifecycle.

**Effort:** Low — Phase 7 is complete; this is an audit + assertion-update task, not a new implementation.

---

### TG-04 — Stress tests not in standard run � High

**Files:** `test/stress/test_edge_cases.c`, `test_memory_exhaustion.c`, `test_sustained_load.c`  
**Problem:** Stress and exhaustion tests live in `test/stress/` but `rtest unit` doesn't run
them. `test_memory_exhaustion.c` and `test_sustained_load.c` are not in the standard build
output (`build/test/`), suggesting they may not compile cleanly against the current API.

**What it solves:** Exercises the allocator under adversarial conditions (exhaustion, sustained
pressure, edge input sizes) — exactly the paths where latent bugs hide.

**Fix:** Build and run all stress tests; fix any compilation failures; add `rtest stress` category.

**Effort:** Low — run existing files, fix any compile errors against current API, wire into `rtest stress`.

---

### TG-05 — No fuzz testing � Medium

**Problem:** Allocation size inputs are only exercised by hand-written test cases. A fuzzer
(libFuzzer or AFL) targeting `Allocator.alloc(random_size)` + `Allocator.dispose` + `realloc`
sequences would find size-boundary bugs and integer overflow paths that deterministic tests miss.

**What it solves:** Catches correctness issues at unusual sizes (0, 1, `SIZE_MAX`, powers-of-2
boundaries, `kAlign` ± 1).

**Approach:** Incremental — add one fuzz target each time a source file is opened for modification.
Start with `Allocator.alloc` size boundaries; expand to `realloc` sequences and `dispose`
corner cases over time.

---

### TG-06 — No test coverage for `Frame.begin_in` / `Frame.end_in` on arenas 🟠 High

**Context:** FT-04 was marked resolved — `Frame.begin_in(scope)` / `Frame.end_in(scope, frame)`
exist in the v0.2.3 API. However, no test suite exercises frames on a user arena scope.

**What it solves:** Confirms the v0.2.3 sub-interface wiring is correct and catches any
regression where `begin_in` on an arena silently operates on the wrong scope.

**Fix:** Create `test/unit/test_arena_frames.c` with cases: frame on arena allocates from arena,
`end_in` bulk-disposes correctly, arena remains valid and allocatable after frame disposal,
`begin_in` on two different arenas independently track frame state.

---

## Architecture / Design Debt

### AD-01 — SYS0 size is fixed at 8KB (no growth path) 🟠 High

**File:** `include/internal/memory.h` — `#define SYS0_PAGE_SIZE 8192`  
**Problem:** The SYS0 system page holds registers, scope table, bootstrap structures, and the
SLB0 first block. Its size is a compile-time constant and cannot grow. As new subsystems are
added (more scopes, more registers, larger scope metadata), SYS0 reserved space shrinks until
it is exhausted.

**What it solves:** Prevents an eventual hard ceiling on system-level growth without requiring a
full memory system redesign.

**Options:** Reserve an explicit growth header in SYS0; consider a two-tier model (fixed SYS0
header + dynamically-allocated extension page for overflow scope table entries).

> **Note:** Dynamic SYS0 growth carries real stability risk — internal paths assume registers and
> page-zero sentinel structures are at fixed addresses. The right upper bound isn't yet known.
> Deferred until real headroom pressure is observed in production use.

---

### AD-02 — Maximum 14 user arenas is a hard-coded structural limit 🟡 Medium

**File:** `include/internal/memory.h` — `scope_table` slots 2-15  
**Problem:** The scope table has 16 fixed slots. SYS0 occupies slot 0, SLB0 slot 1, leaving 14
user arena slots. Creating a 15th arena silently fails. The limit is a direct consequence of
the fixed SYS0 size (AD-01).

**What it solves:** Applications that need more than 14 isolated subsystem arenas are blocked.

**Fix:** Spillover scope table in a dynamically-allocated extension page, or increase SYS0 size
(ties to AD-01).

---

### ~~AD-03~~ — Frame nesting depth limit ✅ Resolved (v0.2.3)

**Resolution:** Nested frames were replaced with a **single active frame per scope**
(`MAX_FRAME_DEPTH = 1` in `include/internal/memory.h`). The R7 stack-of-frames approach was
removed; the `prev`-chain handles scope switching instead. No nesting depth limit applies to
the current design.

---

### AD-04 — `R1`, `R3–R6` registers are reserved but unused 🟢 Low

**File:** `include/internal/memory.h` — `sc_registers`  
**Problem:** Six of eight registers are reserved. R1 was planned to cache the NodePool base
(reducing pointer chasing) and R6 for parent scope, but neither was implemented.

**What it solves:** Using R1 for NodePool base cache would eliminate one pointer dereference per
B-tree operation — measurable in tight allocation loops.

**Context:** R1/R3-R6 are reserved for allocation by **external Sigma.X subsystems**
(Sigma.Tasking and others). They are intentionally unclaimed by `sigma.memory` to leave room
for the broader ecosystem. The R1 NodePool-base cache optimization remains valid but must not
be assigned until the register map across Sigma.X modules is formally defined.

---

### AD-05 — No public error reporting mechanism � High

**Problem:** Allocation failures return `NULL`. There is no structured error code, no last-error
query, and no diagnostic string. Callers cannot distinguish "out of memory", "size too large",
"arena exhausted", or "bad pointer". `fprintf(stderr, ...)` is used internally for fatal errors.

**What it solves:** Improves debuggability in production code where stderr is not visible; enables
callers to implement specific recovery strategies per error type.

**Proposed API:**
```c
typedef enum sc_error {
    SC_OK = 0,
    SC_ERR_OOM,              // Out of memory
    SC_ERR_TOO_LARGE,        // Requested size exceeds page capacity
    SC_ERR_SCOPE_FULL,       // POLICY_FIXED arena exhausted
    SC_ERR_SCOPE_LIMIT,      // Max scopes reached
    SC_ERR_FRAME_DEPTH,      // Max frame nesting reached
    SC_ERR_BAD_PTR,          // dispose/realloc on invalid pointer
} sc_error;

sc_error Memory.last_error(void);
const char *Memory.error_string(sc_error err);
```

**Effort:** Low — add a thread-local `sc_last_error` variable; set it at every `NULL`-returning
path; expose `Memory.last_error()` and `Memory.error_string()`.

---

## Documentation Gaps

### DG-01 — `PHASE7_TEST_AUDIT.md` is a stale planning artifact 🟢 Low

**File:** `docs/PHASE7_TEST_AUDIT.md`  
**Problem:** This document describes Phase 7 test impacts as of February 2026, when several
suites were `⏸️ BLOCKED`. Phase 7 is now complete. The doc shows stale `BLOCKED` status and
does not reflect the current passing state.

**Fix:** Archive (rename to `PHASE7_TEST_AUDIT_ARCHIVED.md`) or update all statuses to reflect
the current passing state.

---

### DG-02 — `v0.2.2_PLAN.md` is an outdated planning document 🟢 Low

**File:** `docs/v0.2.2_PLAN.md` (1039 lines)  
**Problem:** This is a pre-implementation planning document for v0.2.2. Now that v0.2.2 and
v0.2.3 are complete, it no longer reflects reality and adds noise when scanning the docs
directory.

**Fix:** Move to `docs/archive/` or reference it from MEMORY_DESIGN.md as historical context.

---

### DG-03 — No `CHANGELOG.md` (machine-readable release history) 🟡 Medium

**Problem:** Release history lives across `RELEASE_v0.2.x.md` files and `ROADMAP.md`. There is
no single machine-readable changelog in [Keep a Changelog](https://keepachangelog.com) format.
Tools that parse changelogs (npm, GitHub release notes, grepping for a version) cannot process
the current format.

**Fix:** Create `CHANGELOG.md` with `Added / Changed / Fixed / Removed` sections per version,
derived from the existing release notes.

---

## Candidate Items (Deferred Pending Need)

These are potentially useful features that have been explicitly deferred in the ROADMAP until
there is demonstrated demand.

| ID | Feature | Rationale for Deferral |
|----|---------|----------------------|
| CD-01 | `SCOPE_POLICY_BUMP` slab | Pure bump without B-tree — useful for deterministic-latency contexts, but arenas already provide this. Not adding until proven distinct need. |
| CD-02 | Allocation hints / size classes | Pre-segregated pools per size class (jemalloc-style). Beneficial at high allocation rates but adds structural complexity. Profile first. |
| CD-03 | `Memory.snapshot()`/`Memory.restore()` | Full system checkpoint. Powerful for test isolation but invasive to implement. Frames + arenas cover most cases today. |
| CD-04 | `mprotect` guard pages | Allocate guard pages around buffers (`PROT_NONE`) to catch overflows at the OS level. Useful in debug builds; significant overhead. |
| CD-05 | Allocator metrics / telemetry | Prometheus-style counters for `alloc_count`, `dispose_count`, `oom_count`. Useful for production observability but premature for current scale. |

---

## ✅ Resolved Items

| ID | Item | Resolved In |
|----|------|-------------|
| FT-04 | Arena frame support (`Frame.begin_in` / `Frame.end_in`) | v0.2.3 |
| AD-03 | Frame nesting depth limit (replaced with single-frame-per-scope model) | v0.2.3 |

---

## Summary by Priority

| Count | Priority | Items |
|-------|----------|-------|
| 1 | 🔴 Critical | FT-01 |
| 10 | 🟠 High | BG-01, BG-02, FT-02, FT-03, FT-09, TG-01, TG-03, TG-04, TG-06, AD-05 |
| 6 | 🟡 Medium | BG-03, BG-04, FT-05, FT-08, TG-02, TG-05 |
| 8 | 🟢 Low | FT-06, FT-07, FT-10, AD-01, AD-02, AD-04, DG-01, DG-02 |
| 2 | ✅ Resolved | FT-04, AD-03 |
| 5 | ⏸️ Deferred | CD-01 through CD-05 |
