                # Phase 7 Test Impact Audit

**Date:** February 9, 2026  
**Purpose:** Document required changes for each test file during Phase 7 implementation  
**Scope:** Two-tier architecture (skip list + per-page B-trees)

---

## Task Mapping & Status

| Test File | Impact | Relevant Task(s) | Status | Notes |
|-----------|--------|------------------|--------|-------|
| test_bootstrap.c | Minimal | Task 2 (sc_scope update) | ✅ COMPLETE | 7/7 tests pass |
| test_stack.c | Minimal | None | ✅ COMPLETE | No changes needed |
| test_integration.c | Moderate | Task 8 (SLB0 init complete) | ⏸️ BLOCKED | Needs Task 6-7 first |
| test_btree_benchmark.c | Moderate | Task 7 (per-page B-tree) | ⏸️ BLOCKED | Needs Task 6-7 first |
| test_nodepool.c | Major | Task 5 (init) + Task 6 (skip list) | 🔄 IN PROGRESS | Partial (init done) |
| test_btree_ops.c | Major | Task 7 (per-page B-tree) | ⏸️ BLOCKED | Needs Task 6-7 first |
| test_btree_validation.c | Major | Task 7 (per-page B-tree) | ⏸️ BLOCKED | Needs Task 6-7 first |
| test_btree_stress.c | Major | Task 7 (per-page B-tree) | ⏸️ BLOCKED | Needs Task 6-7 first |

**Legend:**
- ✅ COMPLETE: All tests pass, changes finalized
- 🔄 IN PROGRESS: Changes started, tests being updated
- ⏸️ BLOCKED: Waiting on dependency tasks to complete
- ❌ FAILED: Tests failing, requires fixes

---

## Impact Classification

### Minimal Impact (Structure Size Only)
Tests that only validate structure sizes or unrelated functionality.

**test_bootstrap.c** ✅ COMPLETE
- **Relevant Task:** Task 2 (Update sc_scope structure with nodepool_base field)
- **Status:** All changes complete, 7/7 tests passing
- Test Count: 7 tests
- Changes Completed:
  - ✅ Updated SYS0_SLOTS references to SYS0_SLAB_TABLE
  - ✅ Verified `sizeof(sc_scope) == 64` still holds (nodepool_base field)
  - ✅ All structure alignment tests pass
- Effort Spent: 5 minutes

**test_stack.c** ✅ COMPLETE
- **Relevant Task:** None (unaffected by Phase 7 changes)
- **Status:** No changes required, tests pass as-is
- Test Count: 16 tests
- Changes Required: None (stack operations unaffected by NodePool changes)
- Effort Spent: 0 minutes

---

### Moderate Impact (API Updates)
Tests that use changed APIs but don't deeply interact with NodePool internals.

**test_integration.c** ⏸️ BLOCKED
- **Relevant Task:** Task 8 (Update SLB0 initialization in memory_constructor)
- **Status:** Blocked, waiting on Task 6 (skip list) and Task 7 (per-page B-tree) completion
- Test Count: 7 tests
  - test_single_reuse
  - test_multiple_allocs
  - test_interleaved_ops
  - test_varying_sizes
  - test_stress_cycles
  - test_no_leaks
  - test_coalescing

- Changes Required:
  - Update allocation pattern expectations (per-page trees)
  - Verify coalescing works across page boundaries
  - Check that stress tests exercise skip list growth
  
- Key Functions to Update:
  - Memory API calls (should be transparent)
  - Validation assertions (may need adjustment for two-tier lookup)
  
- Estimated Effort: 2-3 hours

**test_btree_benchmark.c** ⏸️ BLOCKED
- **Relevant Task:** Task 7 (Refactor per-page B-tree operations)
- **Status:** Blocked, waiting on Task 6 (skip list) and Task 7 (per-page B-tree) completion
- Test Count: 6 benchmarks
  - test_benchmark_random_insert_1000
  - test_benchmark_sequential_insert_1000
  - test_benchmark_search_1000
  - test_benchmark_mark_free_500
  - test_benchmark_raw_node_allocation_1000
  - test_benchmark_memory_overhead

- Changes Required:
  - Update benchmarks to use per-scope NodePool API
  - Add skip list overhead to memory calculations
  - Adjust performance expectations (two-tier lookup)
  - Add new benchmark: skip list search performance
  
- Estimated Effort: 3-4 hours

---

### Major Impact (Complete Rewrite)
Tests that directly manipulate NodePool and B-Tree internals.

**test_nodepool.c** 🔄 IN PROGRESS
- **Relevant Tasks:** Task 5 (Implement nodepool_init) + Task 6 (Implement skip list operations)
- **Status:** Partial completion - nodepool_init() implemented, skip list tests pending
- Test Count: 13 tests (8 need updates, 5 new tests to add)
  - test_nodepool_initializes
  - test_nodepool_r1_caches_base
  - test_nodepool_free_list_initialized
  - test_nodepool_node_zero_reserved
  - test_nodepool_alloc_single_node
  - test_nodepool_alloc_reduces_free_count
  - test_nodepool_alloc_zeroes_node
  - test_nodepool_get_node_returns_valid_pointer
  - test_nodepool_free_node
  - test_nodepool_free_null_is_noop
  - test_nodepool_free_allows_reallocation
  - test_nodepool_multiple_allocs
  - test_nodepool_alloc_free_interleaved

- Changes Completed:
  - ✅ nodepool_init(scope) API implemented
  - ✅ nodepool_alloc_page_node() implemented
  - ✅ nodepool_alloc_btree_node() implemented
  - ✅ nodepool_get_page_node() implemented
  - ✅ nodepool_get_btree_node() implemented
  
- Changes Pending:
  - ⏸️ COMPLETE REWRITE - API changed from global to per-scope
  - ⏸️ New API: `nodepool_init(scope)` instead of `NodePool.init()`
  - ⏸️ Test skip list operations (insert, find, delete)
  - ⏸️ Test page_node allocation vs btree_node allocation
  - ⏸️ Verify header tracking (page_count, alloc offsets)
  - ⏸️ Test NodePool growth (mremap from 8KB→24KB)
  - ⏸️ Validate collision detection (page_nodes vs btree_nodes)
  
- New Tests to Add:
  - test_nodepool_per_scope_isolation
  - test_nodepool_skip_list_insert
  - test_nodepool_skip_list_find_containing
  - test_nodepool_skip_list_ordered_traversal
  - test_nodepool_growth_via_mremap
  - test_nodepool_collision_detection
  - test_nodepool_header_tracking
  
- Estimated Effort: 8-12 hours (40% complete)

**test_btree_ops.c** ⏸️ BLOCKED
- **Relevant Task:** Task 7 (Refactor per-page B-tree operations)
- **Status:** Blocked, waiting on Task 6 (skip list) and Task 7 (per-page B-tree) completion
- Test Count: Unknown (need to audit)
- Changes Required:
  - **MAJOR REFACTOR** - All operations now per-page, not global
  - Update insert to work with page-local indices
  - Update search to first find page (skip list), then search tree
  - Update delete to handle per-page roots
  - Test cross-page allocation (skip list directs to next page)
  
- New Tests to Add:
  - test_btree_insert_single_page
  - test_btree_insert_across_pages
  - test_btree_search_finds_correct_page
  - test_btree_per_page_tree_isolation
  - test_btree_skip_list_directs_allocation
  
- Estimated Effort: 8-12 hours

**test_btree_validation.c** ⏸️ BLOCKED
- **Relevant Task:** Task 7 (Refactor per-page B-tree operations)
- **Status:** Blocked, waiting on Task 6 (skip list) and Task 7 (per-page B-tree) completion
- Test Count: 10 tests
  - test_validate_empty_tree
  - test_validate_single_node
  - test_validate_clean_tree
  - test_validate_detects_bst_violation
  - test_validate_detects_hint_size_mismatch
  - test_validate_detects_adjacency_violation
  - test_validate_detects_invalid_index
  - test_validate_detects_zero_length
  - test_validate_after_coalesce
  - test_validate_complex_tree

- Changes Required:
  - **MAJOR REFACTOR** - Validation now per-page
  - Update invariant checks for per-page tree roots
  - Add skip list invariant validation
  - Verify page_node.btree_root points to valid tree
  - Check skip list address ordering
  
- New Invariants to Add:
  - Skip list level consistency (forward pointers valid)
  - Page address ordering (ascending)
  - Page isolation (no cross-page node references)
  - Header consistency (page_count matches skip list length)
  
- Estimated Effort: 6-8 hours

**test_btree_stress.c** ⏸️ BLOCKED
- **Relevant Task:** Task 7 (Refactor per-page B-tree operations)
- **Status:** Blocked, waiting on Task 6 (skip list) and Task 7 (per-page B-tree) completion
- Test Count: Unknown (need to audit)
- Changes Required:
  - **MAJOR REFACTOR** - Stress tests must exercise two-tier architecture
  - Generate workloads that span multiple pages
  - Verify skip list performance under load
  - Test NodePool growth during stress
  - Validate no corruption after 10K+ operations
  
- New Stress Tests:
  - test_stress_multi_page_allocation
  - test_stress_skip_list_growth
  - test_stress_nodepool_growth
  - test_stress_two_tier_search
  - test_stress_per_page_coalescing
  
- Estimated Effort: 6-8 hours

---

## Summary Statistics

| Impact Level | Test Files | Test Count | Status | Effort (Est/Actual) |
|--------------|-----------|------------|--------|---------------------|
| **Minimal**  | 2 files   | ~23 tests  | ✅ 2/2 Complete | 5 min / 5 min |
| **Moderate** | 2 files   | ~13 tests  | ⏸️ 0/2 Blocked | 5-7 hrs / 0 hrs |
| **Major**    | 4 files   | ~40+ tests | 🔄 1/4 In Progress | 28-40 hrs / ~3 hrs |
| **TOTAL**    | 8 files   | ~76+ tests | 🔄 3/8 Active | 33-47 hrs / ~3 hrs |

**Progress Breakdown:**
- ✅ **Complete:** test_bootstrap.c (7/7), test_stack.c (16/16)
- 🔄 **In Progress:** test_nodepool.c (~40% complete)
- ⏸️ **Blocked:** 5 files waiting on Task 6 (skip list) and Task 7 (per-page B-tree)

**Next Actions:**
1. Complete Task 6: Implement skip list operations (insert, find, delete)
2. Unblock test_nodepool.c with skip list tests
3. Complete Task 7: Refactor per-page B-tree operations
4. Unblock remaining 4 test files
5. Complete Task 8: Finalize SLB0 initialization

---

## Implementation Strategy

### Phase 1: Infrastructure (Tests Off)
1. Implement new structures (nodepool_header, page_node)
2. Implement nodepool_init(scope)
3. Implement skip list operations
4. Refactor B-Tree for per-page operation
5. Update SLB0 initialization

**During Phase 1:** Most tests will FAIL (expected)

### Phase 2: Minimal Impact Tests (Validate Structures)
1. Fix test_bootstrap.c (verify structure sizes)
2. Verify test_stack.c (should pass unchanged)

**Goal:** Ensure basic structures correct before complex tests

### Phase 3: NodePool Tests (New API)
1. Rewrite test_nodepool.c for per-scope API
2. Add skip list tests
3. Add growth tests
4. Add header validation tests

**Goal:** NodePool operations validated in isolation

### Phase 4: B-Tree Tests (Per-Page Trees)
1. Rewrite test_btree_ops.c for per-page trees
2. Update test_btree_validation.c for new invariants
3. Fix test_btree_stress.c for multi-page scenarios
4. Update test_btree_benchmark.c for two-tier performance

**Goal:** B-Tree operations correct for per-page architecture

### Phase 5: Integration Tests (End-to-End)
1. Fix test_integration.c for two-tier allocation
2. Run full suite with valgrind
3. Verify no memory leaks
4. Benchmark performance vs Phase 6

**Goal:** Full system validation

---

## Test Execution Order

```bash
# Phase 2: Structure validation
$ ctest bootstrap
$ ctest stack

# Phase 3: NodePool isolation
$ ctest nodepool --valgrind

# Phase 4: B-Tree operations
$ ctest btree_ops
$ ctest btree_validation
$ ctest btree_stress --valgrind
$ ctest btree_benchmark

# Phase 5: Full integration
$ ctest integration --valgrind
$ ctest --all --valgrind
```

---

## Risk Assessment

### High Risk Areas
1. **Memory leaks during growth** - mremap invalidates cached pointers
2. **Skip list corruption** - Probabilistic structure easy to break
3. **Per-page isolation** - Cross-page references would segfault
4. **NodePool collision** - page_nodes growing up, btree_nodes growing down

### Mitigation Strategies
1. **Valgrind on every test** - Catch leaks immediately
2. **Skip list invariant checks** - Validate after every insert/delete
3. **Boundary checks** - Assert node indices local to page
4. **Collision detection** - Alert when alloc offsets meet

---

## Completion Criteria

Phase 7 implementation complete when:
- ⏸️ All 8 test files pass (currently 2/8 passing)
- ⏸️ Valgrind reports 0 leaks (tested on bootstrap only)
- ⏸️ Performance within 10% of Phase 6
- ✅ Bootstrap test validates structure sizes (COMPLETE)
- ⏸️ Stress tests run 10K+ operations without corruption

**Current Progress:** 3.5/8 tasks complete
- ✅ Task 1: Documentation (COMPLETE)
- ✅ Task 2: sc_scope structure (COMPLETE)
- ✅ Task 3: Structure definitions (COMPLETE)
- ✅ Task 4: Test audit (COMPLETE)
- ✅ Task 5: nodepool_init (COMPLETE)
- 🔄 Task 6: Skip list operations (IN PROGRESS)
- ⏸️ Task 7: Per-page B-tree refactor (NOT STARTED)
- ⏸️ Task 8: SLB0 initialization update (NOT STARTED)

---

**Status:** IMPLEMENTATION IN PROGRESS (Tasks 1-5 complete, 3 remaining)  
**Next Action:** Complete Task 6 (skip list operations)  
**Last Updated:** February 9, 2026 17:10 UTC

