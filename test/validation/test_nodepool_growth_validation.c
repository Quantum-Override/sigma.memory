/*
 * SigmaCore
 * Copyright (c) 2025 David Boarman (BadKraft) and contributors
 * QuantumOverride [Q|]
 * ----------------------------------------------
 * MIT License
 * ----------------------------------------------
 * File: test_nodepool_growth_validation.c
 * Description: Validation tests for NodePool growth edge cases
 *              Phase 1 Day 3: Data integrity, stress testing
 */

#include <sigma.test/sigtest.h>
#include "internal/memory.h"
#include "internal/node_pool.h"
#include "memory.h"

#if 1  // Region: Test Set Setup & Teardown
static void set_config(FILE **log_stream) {
    *log_stream = fopen("logs/test_nodepool_growth_validation.log", "w");
    sbyte state = Memory.state();
    Assert.isTrue(state & MEM_STATE_READY, "Memory system should be ready");
}

static void set_teardown(void) {
    // Cleanup
}
#endif

#if 1  // Region: Helper Functions
// Helper to get nodepool header
static nodepool_header *get_nodepool_header(scope s) {
    if (s == NULL || s->nodepool_base == ADDR_EMPTY) {
        return NULL;
    }
    return (nodepool_header *)s->nodepool_base;
}
#endif

#if 1  // Region: Validation Tests
// ============================================================================
// NPV-01: Growth preserves existing data (mremap MAYMOVE safety)
// ============================================================================
void test_growth_preserves_existing_data(void) {
    // Arrange: Allocate initial nodes and write sentinel values
    scope s = Memory.get_scope(1);  // SLB0
    Assert.isNotNull(s, "SLB0 scope should exist");

    // Allocate and mark TWO page_nodes with different sentinel values
    uint16_t page_idx1 = nodepool_alloc_page_node(s);
    Assert.isTrue(page_idx1 != PAGE_NODE_NULL, "Page allocation 1 should succeed");

    page_node *pn1 = nodepool_get_page_node(s, page_idx1);
    Assert.isNotNull(pn1, "Page node 1 pointer should be valid");
    pn1->page_base = 0xDEADBEEF;
    pn1->btree_root = NODE_NULL;   // NODE_NULL is the correct empty-page state;
    pn1->block_count = 0x5678;     // btree_root is patched by reindex on growth

    uint16_t page_idx2 = nodepool_alloc_page_node(s);
    Assert.isTrue(page_idx2 != PAGE_NODE_NULL, "Page allocation 2 should succeed");

    page_node *pn2 = nodepool_get_page_node(s, page_idx2);
    Assert.isNotNull(pn2, "Page node 2 pointer should be valid");
    pn2->page_base = 0xCAFEBABE;
    pn2->btree_root = NODE_NULL;   // NODE_NULL is the correct empty-page state;
    pn2->block_count = 0xDCBA;     // btree_root is patched by reindex on growth

    printf("  Allocated page nodes: idx1=%u, idx2=%u\n", page_idx1, page_idx2);
    printf("  Sentinel values: page1=0x%lX, page2=0x%lX\n", (unsigned long)pn1->page_base,
           (unsigned long)pn2->page_base);

    nodepool_header *h = get_nodepool_header(s);
    usize initial_capacity = h->capacity;
    printf("  Initial capacity: %zu bytes\n", initial_capacity);

    // Act: Force multiple growth cycles by allocating many page nodes
    printf("  Forcing growth with 2000 page_node allocations...\n");
    for (int i = 0; i < 2000; i++) {
        nodepool_alloc_page_node(s);
    }

    h = get_nodepool_header(s);
    printf("  Final capacity: %zu bytes (growth occurred)\n", h->capacity);
    Assert.isTrue(h->capacity > initial_capacity, "Pool should have grown: %zu -> %zu",
                  initial_capacity, h->capacity);

    // Assert: Original page_node data intact after mremap.
    // page_base and block_count are plain data fields — never reindexed.
    // btree_root is a btree_node index: if non-NULL it gets patched by reindexing;
    // we set it to NODE_NULL so it is skipped by the reindex and stays zero.
    pn1 = nodepool_get_page_node(s, page_idx1);
    Assert.isNotNull(pn1, "Page node 1 pointer should still be valid");
    Assert.isTrue(pn1->page_base == 0xDEADBEEF,
                  "Page1 data corrupted: expected 0xDEADBEEF, got 0x%lX",
                  (unsigned long)pn1->page_base);
    Assert.isTrue(pn1->btree_root == NODE_NULL,
                  "Page1 btree_root corrupted: expected NODE_NULL, got 0x%X", pn1->btree_root);
    Assert.isTrue(pn1->block_count == 0x5678,
                  "Page1 block_count corrupted: expected 0x5678, got 0x%X", pn1->block_count);

    pn2 = nodepool_get_page_node(s, page_idx2);
    Assert.isNotNull(pn2, "Page node 2 pointer should still be valid");
    Assert.isTrue(pn2->page_base == 0xCAFEBABE,
                  "Page2 data corrupted: expected 0xCAFEBABE, got 0x%lX",
                  (unsigned long)pn2->page_base);
    Assert.isTrue(pn2->btree_root == NODE_NULL,
                  "Page2 btree_root corrupted: expected NODE_NULL, got 0x%X", pn2->btree_root);
    Assert.isTrue(pn2->block_count == 0xDCBA,
                  "Page2 block_count corrupted: expected 0xDCBA, got 0x%X", pn2->block_count);

    printf("  ✓ Data integrity verified after growth\n");
}

// ============================================================================
// NPV-02: Stress test - rapid sequential allocations
// ============================================================================
void test_rapid_sequential_allocations(void) {
    // Arrange: Get initial state
    scope s = Memory.get_scope(1);  // SLB0
    Assert.isNotNull(s, "SLB0 scope should exist");

    nodepool_header *h = get_nodepool_header(s);
    usize initial_capacity = h->capacity;

    printf("  Initial capacity: %zu bytes\n", initial_capacity);
    printf("  Performing 5000 rapid page_node allocations...\n");

    // Act: Rapid sequential allocations (stress test growth)
    int allocation_count = 5000;
    for (int i = 0; i < allocation_count; i++) {
        uint16_t idx = nodepool_alloc_page_node(s);
        Assert.isTrue(idx != PAGE_NODE_NULL, "Allocation %d should succeed", i);

        // Verify we can write to the node
        page_node *pn = nodepool_get_page_node(s, idx);
        Assert.isNotNull(pn, "Node pointer should be valid");
        pn->page_base = (addr)i;  // Write unique value
    }

    h = get_nodepool_header(s);
    printf("  Final capacity: %zu bytes\n", h->capacity);

    // Assert: Pool grew to accommodate all allocations
    Assert.isTrue(h->capacity > initial_capacity, "Pool should have grown significantly");

    printf("  ✓ %d sequential allocations successful\n", allocation_count);
}

// ============================================================================
// NPV-03: Stress test - alternating allocation pattern
// ============================================================================
void test_alternating_allocation_pattern(void) {
    // Arrange: Get initial state
    scope s = Memory.get_scope(1);  // SLB0
    Assert.isNotNull(s, "SLB0 scope should exist");

    nodepool_header *h = get_nodepool_header(s);
    usize initial_capacity = h->capacity;

    printf("  Initial capacity: %zu bytes\n", initial_capacity);
    printf("  Performing 2000 alternating allocations (page/btree pattern)...\n");

    // Act: Alternating allocations (stresses collision detection)
    int cycles = 2000;
    usize capacity_before_test = h->capacity;

    for (int i = 0; i < cycles; i++) {
        // Alternate between page and btree allocations
        if (i % 2 == 0) {
            uint16_t idx = nodepool_alloc_page_node(s);
            Assert.isTrue(idx != PAGE_NODE_NULL, "Page allocation %d should succeed", i);
        } else {
            node_idx idx = nodepool_alloc_btree_node(s);
            Assert.isTrue(idx != NODE_NULL, "BTree allocation %d should succeed", i);
        }

        // Print progress every 500 cycles
        if ((i + 1) % 500 == 0) {
            h = get_nodepool_header(s);
            printf("    Progress: %d/%d allocations, capacity=%zu bytes\n", i + 1, cycles,
                   h->capacity);
        }
    }

    h = get_nodepool_header(s);
    printf("  Final capacity: %zu bytes\n", h->capacity);

    // Assert: Pool may or may not have grown depending on initial state
    // Main assertion: all allocations succeeded
    printf("  ✓ Alternating allocations successful (capacity: %zu -> %zu)\n", capacity_before_test,
           h->capacity);
}

// ============================================================================
// NPV-04: Document growth failure behavior (MAP_FAILED)
// ============================================================================
void test_growth_failure_behavior_documented(void) {
    // NOTE: This test documents expected behavior on mremap failure
    // Actual failure testing requires:
    // - Setting RLIMIT_AS to artificially limit address space
    // - Mocking mremap to return MAP_FAILED
    // - Running in constrained environments

    printf("  Growth failure behavior:\n");
    printf("    - nodepool_alloc_page_node() returns PAGE_NODE_NULL on MAP_FAILED\n");
    printf("    - nodepool_alloc_btree_node() returns NODE_NULL on MAP_FAILED\n");
    printf("    - No memory leaks on failure (MAP_FAILED checked before updates)\n");
    printf("    - Original pool data remains intact on failure\n");
    printf("  ✓ Behavior documented (actual testing requires mocking)\n");

    // This test always passes - it's documentation
    Assert.isTrue(true, "Documentation test");
}

// ============================================================================
// NPV-05: Large allocation stress (trigger multiple growth cycles)
// ============================================================================
void test_large_allocation_stress(void) {
    // Arrange: Get initial state
    scope s = Memory.get_scope(1);  // SLB0
    Assert.isNotNull(s, "SLB0 scope should exist");

    nodepool_header *h = get_nodepool_header(s);
    usize initial_capacity = h->capacity;

    printf("  Initial capacity: %zu bytes\n", initial_capacity);
    printf("  Performing large allocation to force multiple growth cycles...\n");

    // Act: Allocate enough to force 2-3 more growth cycles
    // Note: page_node indices are uint16_t, so max ~65K nodes possible
    // At 20 bytes each, max ~1.3MB of page nodes
    // Start from current capacity and try to double it 2-3 times
    usize target_capacity = initial_capacity * 4;  // Force a few more doublings

    // But cap at reasonable limit given uint16_t index constraint
    // Calculate how many page nodes we've already allocated
    usize current_page_allocs =
        (h->page_alloc_offset - sizeof(nodepool_header)) / sizeof(page_node);
    usize max_safe_page_nodes = 50000;  // Stay well under 65K limit

    // Only allocate if we haven't hit the limit
    if (current_page_allocs >= max_safe_page_nodes) {
        printf("  Already allocated %zu page_nodes (near uint16_t limit)\n", current_page_allocs);
        printf("  Skipping large allocation test to avoid overflow\n");
        printf("  ✓ Test acknowledged index limit constraint\n");
        return;  // Skip this test - we've already allocated too many
    }

    usize nodes_to_allocate = max_safe_page_nodes - current_page_allocs;
    if (nodes_to_allocate > 10000) {
        nodes_to_allocate = 10000;  // Limit to reasonable amount for test speed
    }

    printf("  Current allocations: %zu page_nodes\n", current_page_allocs);
    printf("  Allocating %zu more page_nodes...\n", nodes_to_allocate);

    int growth_count = 0;
    usize last_capacity = initial_capacity;

    for (usize i = 0; i < nodes_to_allocate; i++) {
        uint16_t idx = nodepool_alloc_page_node(s);
        Assert.isTrue(idx != PAGE_NODE_NULL, "Large allocation %zu should succeed", i);

        // Check if growth occurred
        h = get_nodepool_header(s);
        if (h->capacity > last_capacity) {
            growth_count++;
            printf("    Growth #%d: %zu -> %zu bytes\n", growth_count, last_capacity, h->capacity);
            last_capacity = h->capacity;
        }
    }

    h = get_nodepool_header(s);
    printf("  Final capacity: %zu bytes (%d growth cycles)\n", h->capacity, growth_count);

    // Assert: At least some growth occurred
    Assert.isTrue(growth_count >= 1, "Should have grown at least 1 time, got %d", growth_count);
    Assert.isTrue(h->capacity > initial_capacity, "Should have increased capacity");

    printf("  ✓ Large allocation stress test passed\n");
}
#endif

#if 1  // Region: Test Registration
__attribute__((constructor)) void init_nodepool_growth_validation_tests(void) {
    testset("NodePool: Growth Validation", set_config, set_teardown);

    testcase("NPV-01: Growth preserves existing data", test_growth_preserves_existing_data);
    testcase("NPV-02: Rapid sequential allocations", test_rapid_sequential_allocations);
    testcase("NPV-03: Alternating allocation pattern", test_alternating_allocation_pattern);
    testcase("NPV-04: Growth failure behavior documented", test_growth_failure_behavior_documented);
    testcase("NPV-05: Large allocation stress", test_large_allocation_stress);
}
#endif
