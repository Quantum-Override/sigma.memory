# Sigma.Memory - Roadmap

**Current Version:** 0.3.0 (in development — Controller Model rewrite)  
**Last Updated:** March 16, 2026  
**Branch:** main

---

## v0.3.0 — Controller Model 🔨 IN PROGRESS

**Status:** 🔨 Ground-up rewrite — Design complete → see [`docs/design.md`](design.md)  
**Theme:** Deterministic allocation via typed controllers — simplicity over abstraction  
**Strategic Significance:** Fixes three structural problems in v0.2.x; relocates `sc_allocator_i`
to `sigma.core`; makes sigma.text genuinely independent of sigma.memory at link time.

> **Note:** The previously planned v0.3.0 "Trusted Subsystem Registration" design (Sigma.Tasking
> fiber arena integration) is deferred. That design is archived in
> [`../sigma.mem_0.2/docs/plan-v0.3.0.md`](../../sigma.mem_0.2/docs/plan-v0.3.0.md).
> The Controller Model resolves the foundational problems that needed to be fixed first; Tasking
> integration will be re-evaluated once 0.3.0 is stable.

**Key Features:**
- `slab` type: raw mmap-backed region, no embedded policy
- `bump_allocator` (`sc_bump_ctrl_s *`): cursor bump, `reset`, frame snapshot stack (depth 16),
  no individual free — O(1) alloc, O(1) reset
- `reclaim_allocator` (`sc_reclaim_ctrl_s *`): MTIS-backed (skip-list PageList + B-tree NodePool),
  individual `free`, frame sequence-tag sweep — `Allocator.alloc/free/realloc` facade dispatches here
- Controller structs allocated from **SLB0** — no separate bump pool; SLB0 handles reuse on release
- Controller registry: `sc_ctrl_registry_s` embedded in SYS0 (≤ 257 bytes); tracks up to
  `SC_MAX_CONTROLLERS` (32) controller pointers
- R7 permanently fixed to SLB0 — scope stack, `Scope.set/restore` removed
- `sc_allocator_i` definition moves to `sigma.core` — decouples sigma.text from sigma.memory
- Removed: `Allocator.Scope`, `Allocator.Arena`, `Allocator.Resource`, `Allocator.promote`,
  frame depth globals, `sc_resource_i`, `sc_arena_i`, `sc_allocator_scope_i`
- Retained: `Allocator.alloc / free / realloc` top-level facade (dispatches to SLB0 reclaim ctrl)

**Test Plan (TDD — 6 phases):**  
Phase 0: sigma.core interface relocation + sigma.text bool-arg fix  
Phase 1: `slab` type — acquire/release, basic mmap round-trip  
Phase 2: `bump_allocator` — alloc, reset, frame_begin/frame_end, overflow  
Phase 3: `reclaim_allocator` — alloc, free, realloc, frame sequence-tag  
Phase 4: Registry — create_bump/create_reclaim, SLB0 ctrl alloc, shutdown walk  
Phase 5: Public API — Allocator facade, drop-in compat, Valgrind clean  
Phase 6: sigma.text integration — clean build, REQUIRES=("sigma.core") only

**Forward Compatibility:** Control page layout and API are compatible with the future Ring 0
SYS0-DAT static fixture model. No API breaks on migration to OS-level bootstrap.

---

## v0.2.3-realloc - Realloc, Page Release & Skip List Correctness ✅ COMPLETE

**Status:** ✅ Implementation complete, 35/35 tests passing, 0 bytes leaked

**Key Features:**
- ✅ `Allocator.realloc(ptr, size)` — in-place shrink or alloc+copy+dispose grow
- ✅ SLB0 dynamic page release — munmap fully-empty dynamic pages on dispose
- ✅ Skip list correctness fix — fragmented pages not returned for large requests
- ✅ Bug fix: `btree_page_insert` stale pointer after `mremap(MREMAP_MAYMOVE)`
- ✅ Bug fix: `slb0_dispose` page chain dangling `next_page_off` after munmap
- ✅ 3 new unit test suites: `test_realloc` (8), `test_page_release` (5), `test_skiplist_correctness` (4)

**Bug Details:**
- `btree_page_insert`: held `page_node *page` across `nodepool_alloc_btree_node` which may call `mremap(MREMAP_MAYMOVE)`. Fix: re-fetch page pointer after the alloc call.
- `slb0_dispose`: when munmap'ing a freed dynamic page, the predecessor's `next_page_off` was left pointing to unmapped memory. Fix: unlink from page chain before munmap.

**Test Suites Added:**
- `test/unit/test_realloc.c` — REA-01..08: NULL→alloc, NULL+0, zero-size dispose, shrink, grow+data, grow→shrink, split threshold, independent chains
- `test/unit/test_page_release.c` — PRL-01..05: initial pages, dynamic overflow, page release on empty, baseline stability, post-release usability
- `test/unit/test_skiplist_correctness.c` — SLC-01..04: B-tree reuse, coalesced free, Task 7 regression guard, full-cycle re-alloc

**Documentation:**
- ✅ Users Guide: `Allocator.realloc` added to API table
- ✅ MEMORY_REFERENCE.md: `Allocator.realloc` added to API quick-reference
- ✅ All tests validated under valgrind, 0 bytes leaked

---

## v0.2.2-arenas - Arena System ✅ COMPLETE

**Status:** ✅ Implementation complete, 31/31 tests passing, 0 bytes leaked

**Key Features:**
- ✅ User arenas: 14 concurrent scopes (scope_id 2-15)
- ✅ Simple bump allocation: O(1), no metadata overhead
- ✅ NodePool dynamic growth: 8KB→16KB→32KB via mremap
- ✅ Arena API: `create_arena(name, policy)`, `dispose_arena(scope)`
- ✅ Bulk disposal: munmap all pages + NodePool shutdown
- ✅ 31 tests across 7 test files (unit, validation, integration)

**Implementation:**
- Days 1-3: NodePool growth (page_node, btree_node, validation) - 11 tests ✅
- Days 4-6: Arena lifecycle (create, dispose, allocate) - 15 tests ✅
- Day 7: Integration & stress tests - 5 tests ✅

**Performance:**
- Arena allocation: O(1) bump pointer (vs O(log n) for SLB0)
- Arena disposal: O(P) where P = page count (bulk munmap)
- Zero per-allocation metadata overhead
- 8KB pages with 8128 bytes usable (99.2% efficiency)

**Documentation:**
- ✅ Users Guide: Arena API with examples
- ✅ MEMORY_REFERENCE.md: Technical architecture updated
- ✅ All tests documented and validated

**Deprecations:**
- Frame API removed (v0.2.1) - replaced by arena system
- Frame-specific constants removed
- Chunked allocation removed

---

## v0.2.1-frames - Frame Support ✅ DEPRECATED

**Why the pivot:**
- Bitmap approach hit fundamental design issues (disposal size detection)
- In-page metadata prevents 100% page utilization
- B-Tree external tracking = true OS-level allocator model

**Core Innovation:**
- User pages: **100% payload** (no sentinels, headers, bitmaps)
- All allocation metadata: external B-Tree nodes (18 bytes each)
- Per-slab B-Tree roots (selective complexity)
- Stack-based operations (register machine model)

**Key Metrics:**
- SYS0: 4KB → **8KB** (6.6KB DAT available)
- NodePool: **18KB** separate mmap (1,024 nodes)
- Node size: **18 bytes** (start, length, children, hint)
- Page utilization: **100%**

---

## v0.2.0 Implementation Plan (TDD-Driven)

**Development Philosophy:**
1. Write test first (red)
2. Implement minimal passing code (green)
3. Refactor for clarity (refactor)
4. Move to next test

**Test Coverage Target:** 90%+ line coverage, all allocation paths tested


---

## 🎯 Phase 1: Foundation (Bootstrap SYS0 @ 8KB)

**Goal:** Update SYS0 from 4KB → 8KB, verify bootstrap tests pass

**TDD Steps:**
1. **Test:** Update `test_bootstrap.c` for 8KB expectations
   - `SYS0_PAGE_SIZE = 8192`
   - `FIRST_BLOCK_OFFSET = 1536`
   - `LAST_FOOTER_OFFSET = 8184`
2. **Code:** Update `include/internal/memory.h` constants
3. **Code:** Update `src/memory.c` initialization
4. **Verify:** `ctest bootstrap --valgrind` → 8/8 passing

**Structures Changed:**
- Remove: `sc_page_sentinel`, `sc_free_block` (obsolete)
- Add: `sc_node` (18 bytes), `node_idx`, NodeTable layout
- Update: SYS0 offsets (NodeTable @ 1320, NodeStack @ 1350)

**Deliverable:** ✅ Bootstrap tests pass with 8KB SYS0, node structure defined

---

## 🎯 Phase 2: NodePool & Stack Infrastructure

**Goal:** Initialize NodePool, R1 register caching, stack operations

**TDD Steps:**
1. **Test:** `test_node_pool_initialized()` - verify 18KB mmap, R1 set
2. **Code:** Create `src/node_pool.c` - `init_node_pool()`
3. **Test:** `test_node_alloc_from_freelist()` - allocate/free nodes
4. **Code:** Implement `node_alloc()`, `node_free()`
5. **Test:** `test_node_stack_operations()` - push/pop/peek
6. **Code:** Create `src/node_stack.c` - stack primitives
7. **Verify:** All node/stack tests passing

**New Files:**
- `include/internal/node_pool.h` - NodePool interface
- `src/node_pool.c` - 18KB mmap, freelist, get_node()
- `include/internal/node_stack.h` - Stack interface (16 slots)
- `src/node_stack.c` - Stack operations

**Key Functions:**
- `init_node_pool()` - mmap 18KB, initialize freelist
- `node_alloc()` / `node_free()` - freelist management
- `get_node(idx)` - R1 + (idx * 18) → node pointer
- `stack_push(idx, meta)` / `stack_pop()` - operations

**Deliverable:** ✅ NodePool operational, stack working, 1024 nodes available

---

## 🎯 Phase 3: B-Tree Core Operations

**Goal:** Implement BST primitives (search, insert, delete)

**TDD Steps:**
1. **Test:** `test_btree_insert_single()` - insert one node
2. **Code:** Implement `btree_insert(root, start, length)`
3. **Test:** `test_btree_search_by_addr()` - find by address
4. **Code:** Implement `btree_search(root, addr)`
5. **Test:** `test_btree_delete_node()` - remove node
6. **Code:** Implement `btree_delete(root, addr)`
7. **Test:** `test_btree_hint_calculation()` - log2(max_free)
8. **Code:** Implement `btree_update_hints(idx)`
9. **Verify:** All BST operations correct

**New Files:**
- `test/test_btree.c` - B-Tree unit tests
- `include/internal/btree.h` - B-Tree interface
- `src/btree.c` - Tree operations

**Critical Functions:**
- `btree_insert(root, start, length)` - add node, return new root
- `btree_search(root, addr)` - find node by start address
- `btree_delete(root, addr)` - remove node, return new root
- `btree_update_hints(idx)` - recalculate log2 hints bottom-up

**Deliverable:** ✅ BST working in isolation, hints correct, tree validated

---

## 🎯 Phase 4: Node Operations (Stack-Based)

**Goal:** Implement split/merge using stack + register model

**TDD Steps:**
1. **Test:** `test_node_split_oversized()` - split large free block
2. **Code:** Implement `nodes_split()` - pops stack, pushes result
3. **Test:** `test_node_merge_adjacent()` - coalesce neighbors
4. **Code:** Implement `nodes_merge()` - pops 2 nodes, merges
5. **Test:** `test_btree_first_fit_hint()` - hint-guided search
6. **Code:** Implement `btree_find_first_fit(root, size)`
7. **Verify:** Operations use stack, results in R2

**New Files:**
- `include/internal/node_ops.h` - Operation interface
- `src/node_ops.c` - Stack-based split/merge

**Operation Pattern:**
```c
// Caller: push parameters, call operation, get result from R2
stack_push(target_idx, alloc_size);
nodes_split();
node_idx result = Registers.get(R2);
```

**Key Functions:**
- `nodes_split()` - split oversized node, remainder reinserted
- `nodes_merge()` - coalesce adjacent free nodes
- `btree_find_first_fit(root, size)` - log2 hint-guided search

**Deliverable:** ✅ Split/merge working, first-fit efficient, stack-based

---

## 🎯 Phase 5: SLB0 Allocation (B-Tree Backend)

**Goal:** Wire SLB0 to use B-Tree for allocation tracking

**TDD Steps:**
1. **Test:** `test_slb0_basic_alloc()` - allocate from SLB0
2. **Code:** Update `slb0_alloc()` to use `btree_find_first_fit()`
3. **Test:** `test_slb0_dispose()` - free allocation
4. **Code:** Update `slb0_dispose()` to use `btree_search()`, mark free
5. **Test:** `test_slb0_alloc_after_dispose()` - reuse freed space
6. **Code:** Ensure coalescing on dispose
7. **Test:** `test_slb0_multiple_allocations()` - stress test
8. **Verify:** SLB0 fully functional with B-Tree

**Modified Files:**
- `src/memory.c` - rewrite `slb0_alloc()` / `slb0_dispose()`
- `test/test_slb0_btree.c` - NEW test file

**Allocation Flow:**
```
slb0_alloc(size) →
  1. Get root: NodeTable[0]
  2. btree_find_first_fit(root, size)
  3. If found: nodes_split(), mark allocated
  4. If not: mmap new page, create node, insert
  5. Return node->start
```

**Disposal Flow:**
```
slb0_dispose(ptr) →
  1. Get root: NodeTable[0]
  2. btree_search(root, ptr)
  3. Mark node free (bit 9)
  4. nodes_merge() with neighbors
  5. If page empty: munmap, delete node
```

**Deliverable:** ✅ SLB0 working with B-Tree, allocation/disposal correct

---

## 🎯 Phase 6: Diagnostics & Validation

**Goal:** Create test hooks for internal inspection

**TDD Steps:**
1. **Test:** Use diagnostics in all existing tests
2. **Code:** Implement diagnostic functions
3. **Test:** `test_btree_validate_structure()` - integrity check
4. **Code:** Implement tree validator
5. **Verify:** All tests use diagnostics, no hidden bugs

**New Files:**
- `include/internal/diagnostics.h` - Test inspection API
- `src/diagnostics.c` - Implementation

**Diagnostic Functions:**
```c
// NodePool
usize diag_node_pool_capacity(void);
usize diag_node_pool_allocated(void);
node_idx diag_node_freelist_head(void);

// B-Tree
usize diag_btree_height(node_idx root);
usize diag_btree_node_count(node_idx root);
bool diag_btree_validate(node_idx root);  // Check BST property
void diag_btree_dump(node_idx root);      // Pretty-print tree

// Slab
usize diag_slab_page_count(sbyte slab_id);
usize diag_slab_alloc_count(sbyte slab_id);
```

**Deliverable:** ✅ Full observability, tests validate internal state

---

## 🎯 Phase 7: Integration & Stress Testing

**Goal:** End-to-end validation, edge cases, production readiness

**TDD Steps:**
1. **Test:** `test_alloc_1000_blocks()` - high-volume allocation
2. **Test:** `test_random_alloc_pattern()` - chaos testing
3. **Test:** `test_fragmentation_worst_case()` - adversarial patterns
4. **Test:** `test_page_release_threshold()` - verify release logic
5. **Verify:** Valgrind clean (0 leaks, 0 errors)

**New Files:**
- `test/test_integration.c` - End-to-end scenarios

**Test Scenarios:**
- Allocate/free 1000+ blocks random sizes
- Allocate in ascending order, free in descending (fragmentation)
- Interleave small/large allocations
- Stress NodePool growth (exhaust 1024 nodes)
- Verify hint maintenance under load

**Deliverable:** ✅ Production-ready, stress-tested, valgrind clean

---

## 📋 Post-0.2.0 Roadmap

Items for future releases, roughly prioritized:

### High Priority (v0.2.1 Focus)
| ID | Item | Description | Target | Status |
|----|------|-------------|--------|--------|
| F-01 | Frame Support (SLB0) | Frame ops for SLB0 with chunk-based bump allocator | v0.2.1 | ✅ Complete |
| F-02 | Standard Arena/Frame API | User-facing frame create/dispose/introspect | v0.2.1 | ✅ Complete |
| F-03 | Thread-Friendly Architecture | Design hooks for external task management (Sigma.Tasking) | v0.2.1 | Pending |
| F-04 | Arena Extensions | Multi-slab frame support, nested frames | v0.2.1 | Pending |

### Medium Priority (v0.3.0+)
| ID | Item | Description | Target |
|----|------|-------------|--------|
| F-05 | User Arenas (SLB1-14) | Custom scopes, POLICY_RECLAIMING or BUMP | v0.3.0 |
| F-06 | Thread-Safety Implementation | Lock strategy with Sigma.Tasking integration | v0.3.0 |
| F-07 | sys0_dispose Coalescing | Implement block merging for SYS0 | v0.3.0 |
| F-08 | Tree Rebalancing | AVL or RB-tree for pathological cases | v0.3.0+ |

### Low Priority (Future)
| ID | Item | Description | Target |
|----|------|-------------|--------|
| F-09 | Frame Checkpoints (Advanced) | Transactional save/restore for nested scopes | v0.3.0+ |
| F-10 | Scope Introspection (Full) | Stats API (page_count, alloc_count, fragmentation) | v0.3.0+ |
| F-11 | BUMP Policy Slabs | Simple bump allocator for deterministic use cases | v0.3.0+ |
| F-12 | Allocation Hints | Size classes, burst patterns, best-fit vs first-fit | v0.3.0+ |

### Ideas / Exploration
| ID | Item | Description | Notes |
|----|------|-------------|-------|
| I-01 | Log2 Size Classes | Bucketing for faster search (2^n bins) | Post-frames |
| I-02 | Per-Task Arena Contexts | Task-local slab assignments (Sigma.Tasking integration) | Requires F-06 |
| I-03 | Memory Compaction | Defragment by moving allocations (handle-based) | Complex, far future |
| I-04 | Debug Poisoning | Fill freed memory with 0xDEADBEEF | Useful for debugging |
| I-05 | Scope Callbacks | Hooks for profiling (compile-guarded) | v0.3.0+ |

---

## 🏗️ Design Decisions (B-Tree Architecture)

Intentional architectural choices for the v0.2.0 rewrite:

| ID | Decision | Rationale |
|----|----------|-----------|
| DT-01 | 18-byte nodes (no padding) | Efficient: 1024 nodes in 18KB; no wasted space |
| DT-02 | Log2 hint (8 bits) | Represents 2^0 to 2^255 bytes; excellent for bucketing |
| DT-03 | FREE_FLAG in hint field | No separate flag byte; pack into existing field |
| DT-04 | Stack-based operations | Register machine model; reduces ABI overhead |
| DT-05 | Per-slab B-Trees | Selective complexity; BUMP slabs skip tree entirely |
| DT-06 | 100% page utilization | Zero in-page metadata = true OS allocator model |
| DT-07 | External NodePool (18KB) | Separate mmap; remappable for growth |
| DT-08 | R1 for NodePool base | Fast index→pointer translation (no function call) |
| DT-09 | First-fit (hint-guided) | Balance search time vs fragmentation |
| DT-10 | No tree balancing (v0.2.0) | Defer complexity; validate need with real workloads |

---

## 🧪 TDD Best Practices

**Red-Green-Refactor Cycle:**
1. **Red:** Write failing test (defines contract)
2. **Green:** Minimal code to pass test (no gold-plating)
3. **Refactor:** Improve clarity without changing behavior
4. Repeat

**Test Organization:**
- One test file per module (`test_btree.c`, `test_node_pool.c`)
- Descriptive test names: `test_btree_insert_single()` vs `test_insert()`
- Use diagnostic functions to validate internal state
- Valgrind on every test run (`ctest --valgrind`)

**Coverage Goals:**
- Line coverage: 90%+
- Branch coverage: 85%+
- All allocation paths tested (success, failure, edge cases)

**Test Pyramid:**
```
     /\
    /  \      Integration (few)
   /────\     - test_integration.c
  /      \    - End-to-end scenarios
 /────────\   Component (many)
/          \  - test_btree.c, test_node_pool.c
────────────  - Module-level functionality
              Unit (most)
              - Individual functions
              - Edge cases, error paths
```

---

## 📦 Releases

### v0.1.0 ✅ (January 2026)

**Branch:** master  
**Theme:** Bootstrap Foundation

**Delivered:**
- SYS0 bootstrap (4KB static page)
- Registers R0-R7 with scope caching
- scope_table[16] unified design
- SLB0 user allocator (16 pages, hybrid bump + free list)
- Page release on empty
- 30 tests passing (10 bootstrap + 20 slab0)
- Documentation: MEMORY_DESIGN, MEMORY_REFERENCE, Users Guide

---

### v0.2.0-alpha ✅ (February 12, 2026)

**Branch:** rel-0.2.0-btree  
**Tag:** v0.2.0-alpha  
**Theme:** Metadata-External B-Tree Architecture

**Major Changes:**
- Complete redesign: bitmap → B-Tree external tracking
- SYS0: 4KB → 8KB (6.6KB DAT)
- NodePool: 2KB initial (grows dynamically: 2→4→8→16→32KB)
- Node size: 24 bytes (cache-aligned, explicit reserved space)
- Pages: 100% payload (no metadata)
- Stack-based operations (register machine)
- Per-slab B-Tree roots

**Critical Bugfix:**
- Block splitting reuses existing node (prevents pool exhaustion)
- 40 iterations stable in 2KB pool vs 3.5 iterations before

**Performance:**
- 64B blocks: ~1.5M ops/sec
- 1KB blocks: ~3.7M ops/sec
- Mixed workload: ~5.7M ops/sec
- 2-3x improvement over pre-bugfix state

**Test Coverage:**
- 51 tests passing (bootstrap, btree, integration, validation, performance, frames)
- Valgrind clean (no leaks, no errors)

**Package:**
- sigma.memory.o published to /usr/local/packages/
- Ready for SigmaTest integration

**Phases:**
- [x] Phase 0: Re-branch from v0.1.0, update docs, clean tests
- [x] Phase 1: 8KB SYS0, node structure defined
- [x] Phase 2: NodePool + Stack infrastructure
- [x] Phase 3: B-Tree core operations
- [x] Phase 4: Node split/merge ops
- [x] Phase 5: SLB0 wired to B-Tree
- [x] Phase 6: Diagnostics + validation
- [x] Phase 7: Integration + stress testing

**Status:** ✅ COMPLETED (February 12, 2026)

---

### v0.2.1-frames ✅ (Completed - March 8, 2026)

**Theme:** Frame Support for SLB0

**Delivered:**
- ✅ Frame operations (begin/end/depth/allocated) - 17/17 tests passing
- ✅ Hybrid allocation (≤4KB=bump, >4KB=B-tree tracked)
- ✅ Chunked bump allocators (4KB chunks, automatic chaining)
- ✅ LIFO nesting (MAX_DEPTH=16)
- ✅ Large allocation tracking (>4KB allocations owned by frames)
- ✅ Valgrind clean (no leaks, no corruption)

**Status:** Production-ready for SLB0, ready for dog-fooding

---

### v0.2.2 (Next - March 2026) ⚡ CRITICAL PATH

**Theme:** Dog-Food Release - Arena Support for Sigma.Test & Anvil

**Mission:** Get minimal viable multi-arena support into the hands of Sigma.Test and Anvil teams to discover real-world requirements.

**Critical Blockers:**
1. **NodePool Growth (mremap)** - MUST implement
   - Currently stubbed at node_pool.c:348, :382
   - 8KB → 16KB → 32KB → 64KB automatic growth
   - Multi-arena will exhaust 8KB pool quickly
   - **Status:** Week 1 priority ⚡

2. **User Arenas (SLB1-15)** - MUST implement
   - Arena.create/destroy API
   - Per-arena allocation/disposal
   - Per-arena frames (extend v0.2.1 model)
   - POLICY_RECLAIMING only (B-Tree backed)
   - **Status:** Week 1-2 implementation

3. **Thread-Friendly Hooks** - Design only
   - Document coordination points for Sigma.Tasking
   - No locks, no thread-safety (intentional)
   - Hook architecture for external task management
   - **Status:** Week 2 documentation

**Deliverables:**
- ✅ NodePool automatically grows via mremap
- ✅ 14 user arenas available (SLB1-14)
- ✅ Arena create/destroy/allocate/dispose API
- ✅ Per-arena frame support
- ✅ Thread-friendly hooks documented (NOT implemented)
- ✅ 60-70 tests passing (arena lifecycle, growth, isolation)
- ✅ Integration examples for Sigma.Test and Anvil

**Success Criteria:**
- Sigma.Test can run tests in isolated arenas
- Anvil can use per-module/per-phase arenas
- Dog-food feedback informs v0.3.0 priorities

**Timeline:** 3 weeks (target: March 29, 2026)

**Deferred to post-dog-food:**
- POLICY_BUMP/FIXED (wait for proven need)
- Thread-safety implementation (Sigma.Tasking not ready)
- Advanced frame features (transactional, checkpoints)

---

### v0.3.0 (Future - Q2 2026)

**Theme:** Production Hardening - Informed by Dog-Food Feedback

**Scope determined by Sigma.Test & Anvil usage:**
- Additional policies if needed (POLICY_BUMP, POLICY_FIXED)
- Thread-safety implementation (when Sigma.Tasking ready)
- Performance optimizations based on profiling
- Advanced frame features if basic frames insufficient
- sys0_dispose coalescing (if fragmentation proves problematic)
- Tree rebalancing (if pathological cases observed)

**Planning:**
- Week 1 (post-v0.2.2): Collect dog-food feedback
- Week 2: Prioritize based on real-world pain points
- Week 3-6: Implement highest-priority items
- Week 7-8: Validation + production release

**Philosophy:** Don't build what we don't need. Let real usage drive priorities.

---

## 🐛 Known Issues

### v0.2.1 (Current)
| ID | Issue | Status | Notes |
|----|-------|--------|-------|
| KI-04 | Not thread-safe | Tracked | Thread-friendly design pending, implementation in v0.3.0 |
| KI-05 | Single-threaded only | Tracked | Will be managed by Sigma.Tasking (v0.3.0) |

### Resolved Issues
| ID | Issue | Status | Resolution | Version |
|----|-------|--------|------------|----------|
| KI-01 | Bootstrap tests outdated | ✅ Fixed | Updated for 8KB SYS0 | v0.2.0 |
| KI-02 | Node pool exhaustion | ✅ Fixed | Dynamic growth (2→32KB), split reuses nodes | v0.2.0 |
| KI-03 | No frame support | ✅ Fixed | Implemented chunk-based frames for SLB0 | v0.2.1 |
| KI-06 | shutdown_memory_system use-after-free | ✅ Fixed | Fixed page chain traversal ordering | v0.2.1 |

---

## 📚 References

**Documentation:**
- [MEMORY_DESIGN.md](MEMORY_DESIGN.md) - v2.0 Architecture (B-Tree)
- [MEMORY_REFERENCE.md](MEMORY_REFERENCE.md) - API Reference (needs update)
- [BUILDING.md](../BUILDING.md) - Build instructions

**Related Projects:**
- [sigma.core](https://github.com/Quantum-Override/sigma.core) - Type definitions
- [sigma.collections](https://github.com/Quantum-Override/sigma.collections) - Data structures

---

## Notes

### Item ID Conventions
- `F-XX` - Features (new functionality)
- `I-XX` - Ideas (exploration, future concepts)
- `DT-XX` - Design Trade-offs (intentional decisions)
- `KI-XX` - Known Issues (tracked bugs/limitations)

### Updating Roadmap
After each phase:
1. Mark completed items with ✅
2. Update status in issue tracker
3. Reassess priorities based on blockers/discoveries
4. Add new items as needed

---

**Last Updated:** March 8, 2026  
**Current Milestone:** v0.2.1 (Frame Support - Phase 1 Complete ✅)  
**Next Milestone:** v0.2.2 - Thread-Friendly Architecture & Multi-Slab Frames

