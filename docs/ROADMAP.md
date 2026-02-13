# SigmaCore Memory - Roadmap

**Current Version:** 0.2.0-alpha (B-Tree Architecture) ✅  
**Last Updated:** February 12, 2026  
**Branch:** rel-0.2.0-btree (from v0.1.0 baseline)

---

## Architecture Change: Metadata-External B-Tree Model

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
| ID | Item | Description | Target |
|----|------|-------------|--------|
| F-01 | Frame Support (Prototyping) | Frame ops for dynamic/reclaiming slabs | v0.2.1 |
| F-02 | Standard Arena/Frame API | User-facing frame create/dispose/introspect | v0.2.1 |
| F-03 | Thread-Safety Core | Lock strategy, atomic ops, safety analysis | v0.2.1 |
| F-04 | Concurrency Performance | Multi-threaded benchmarks, contention fixes | v0.2.1 |

### Medium Priority (v0.3.0+)
| ID | Item | Description | Target |
|----|------|-------------|--------|
| F-05 | User Arenas (SLB1-14) | Custom scopes, POLICY_RECLAIMING or BUMP | v0.3.0 |
| F-06 | sys0_dispose Coalescing | Implement block merging for SYS0 | v0.3.0 |
| F-07 | Tree Rebalancing | AVL or RB-tree for pathological cases | v0.3.0+ |

### Low Priority (Future)
| ID | Item | Description | Target |
|----|------|-------------|--------|
| F-08 | Frame Checkpoints (Advanced) | Transactional save/restore for nested scopes | v0.3.0+ |
| F-09 | Scope Introspection (Full) | Stats API (page_count, alloc_count, fragmentation) | v0.3.0+ |
| F-10 | BUMP Policy Slabs | Simple bump allocator for deterministic use cases | v0.3.0+ |
| F-11 | Allocation Hints | Size classes, burst patterns, best-fit vs first-fit | v0.3.0+ |

### Ideas / Exploration
| ID | Item | Description | Notes |
|----|------|-------------|-------|
| I-01 | Log2 Size Classes | Bucketing for faster search (2^n bins) | Post-concurrency |
| I-02 | Thread-Local Caches | Per-thread NodePool for lock-free allocs | Related to F-03 |
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
- Documentation: MEMORY_DESIGN, MEMORY_REFERENCE, USERS_GUIDE

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
- 45 tests passing (bootstrap, btree, integration, validation, performance)
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

### v0.2.1 (Next - Q1 2026)

**Theme:** Frame Support & Concurrency Foundation

**Focus Areas:**
1. **Frame Support in Prototyping**
   - Frame operations for dynamic slabs
   - Frame support in reclaiming slabs
   - Checkpoint/restore mechanics
   - Nested frame validation

2. **Standard Arena/Frame API**
   - User-facing frame creation API
   - Frame disposal (rollback allocations)
   - Frame introspection (allocation count, memory usage)
   - Integration with existing slab policies

3. **Concurrency & Thread-Safety**
   - Thread-safety analysis of current implementation
   - Lock strategy design (per-slab vs global)
   - Atomic operations for critical sections
   - Thread-local caching exploration

4. **Performance from Threading**
   - Benchmark single-threaded baseline
   - Multi-threaded allocation stress tests
   - Identify and resolve contention points
   - Scalability validation (2, 4, 8 threads)

**Deliverables:**
- Frame API in prototyping mode (basic operations working)
- Concurrency safety assessment with recommendations
- Thread-safety implementation for core allocator
- Performance baseline: single vs multi-threaded
- Updated benchmarks and stress tests

**Success Criteria:**
- SigmaTest dogfooding reveals no critical bugs
- Frame operations validated in real-world scenarios
- Thread-safe allocations with minimal contention
- Performance scales reasonably with thread count

---

### v0.3.0 (Future - Q2 2026)

**Theme:** Production Multi-Slab & Advanced Policies

**Planned:**
- User arenas (SLB1-SLB14) - production ready
- POLICY_BUMP slabs (deterministic allocation)
- POLICY_RECLAIMING slabs (B-Tree backed)
- Scope introspection API (full stats)
- sys0_dispose coalescing
- Advanced frame operations (transactional, nested)

---

## 🐛 Known Issues

### v0.2.0-alpha (Current)
| ID | Issue | Status | Notes |
|----|-------|--------|-------|
| KI-03 | No frame support yet | Tracked | Planned for v0.2.1 |
| KI-04 | Not thread-safe | Tracked | Planned for v0.2.1 |
| KI-05 | Single-threaded only | Tracked | Concurrent access will corrupt state |

### v0.2.0-alpha (Resolved)
| ID | Issue | Status | Resolution |
|----|-------|--------|------------|
| KI-01 | Bootstrap tests outdated | ✅ Fixed | Updated for 8KB SYS0 |
| KI-02 | Node pool exhaustion | ✅ Fixed | Dynamic growth (2→32KB), split reuses nodes |

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

**Last Updated:** February 12, 2026  
**Current Milestone:** v0.2.0-alpha RELEASED ✅  
**Next Milestone:** v0.2.1 - Frame Support & Concurrency

