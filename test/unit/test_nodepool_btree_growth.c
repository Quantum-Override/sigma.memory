/*
 * SigmaCore
 * Copyright (c) 2025 David Boarman (BadKraft) and contributors
 * QuantumOverride [Q|]
 * ----------------------------------------------
 * MIT License
 * ----------------------------------------------
 * File: test_nodepool_btree_growth.c
 * Description: Tests for NodePool btree_node growth via mremap
 *              Phase 1 Day 2: NPB-01, NPB-02, NPB-03
 */

#include <sigma.test/sigtest.h>
#include "internal/memory.h"
#include "internal/node_pool.h"
#include "memory.h"

#if 1  // Region: Test Set Setup & Teardown
static void set_config(FILE **log_stream) {
    *log_stream = fopen("logs/test_nodepool_btree_growth.log", "w");
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

#if 1  // Region: NPB - NodePool BTree Node Tests
// ============================================================================
// NPB-01: Test btree_node exhaustion NO LONGER OCCURS (mremap prevents it)
// ============================================================================
void test_btreenode_exhaustion_prevented_by_growth(void) {
    // Arrange: Get SLB0 and calculate initial btree_node capacity
    scope s = Memory.get_scope(1);  // SLB0
    Assert.isNotNull(s, "SLB0 scope should exist");

    nodepool_header *h = get_nodepool_header(s);
    Assert.isNotNull(h, "NodePool header should exist");

    // Record initial capacity (pool may have already grown from previous tests)
    usize initial_capacity = h->capacity;
    usize available_space = initial_capacity - sizeof(nodepool_header);
    uint16_t max_btree_nodes = available_space / sizeof(sc_node);

    printf("  Initial capacity: %zu bytes\n", initial_capacity);
    printf("  Max btree_nodes (theoretical): %u\n", max_btree_nodes);

    // Act: Allocate 2x theoretical capacity to force growth
    uint16_t target = max_btree_nodes * 2;
    printf("  Attempting %u allocations (2x capacity)...\n", target);

    for (uint16_t i = 0; i < target; i++) {
        node_idx idx = nodepool_alloc_btree_node(s);
        Assert.isTrue(idx != NODE_NULL, "BTree allocation %u should succeed with mremap (got NULL)",
                      i);
    }

    // Assert: Pool should have grown
    h = get_nodepool_header(s);
    printf("  Final capacity: %zu bytes (growth occurred)\n", h->capacity);
    Assert.isTrue(h->capacity > initial_capacity, "Capacity should have grown: %zu -> %zu",
                  initial_capacity, h->capacity);
}

// ============================================================================
// NPB-02: Test btree_node automatic growth via mremap
// ============================================================================
void test_btreenode_growth_via_mremap(void) {
    // Arrange: Get current capacity
    scope s = Memory.get_scope(1);  // SLB0
    Assert.isNotNull(s, "SLB0 scope should exist");

    nodepool_header *h = get_nodepool_header(s);
    usize initial_capacity = h->capacity;

    // Calculate how many btree_nodes to allocate to force next growth
    usize available_space = initial_capacity - sizeof(nodepool_header);
    uint16_t safe_btree_capacity = available_space / sizeof(sc_node);
    uint16_t forcing_allocation = safe_btree_capacity + 100;  // Force growth

    printf("  Initial capacity: %zu bytes\n", initial_capacity);
    printf("  Attempting %u allocations (safe=%u, forcing growth)...\n", forcing_allocation,
           safe_btree_capacity);

    // Act: Allocate many btree_nodes beyond current capacity
    for (uint16_t i = 0; i < forcing_allocation; i++) {
        node_idx idx = nodepool_alloc_btree_node(s);
        Assert.isTrue(idx != NODE_NULL, "BTree allocation %u should succeed with auto-growth", i);
    }

    // Assert: Capacity doubled
    h = get_nodepool_header(s);
    printf("  Final capacity: %zu bytes\n", h->capacity);
    Assert.isTrue(h->capacity == initial_capacity * 2,
                  "Capacity should double: %zu -> %zu (got %zu)", initial_capacity,
                  initial_capacity * 2, h->capacity);
}

// ============================================================================
// NPB-03: Test mixed page_node and btree_node growth
// ============================================================================
void test_mixed_pagenode_btreenode_growth(void) {
    // Arrange: Get initial capacity
    scope s = Memory.get_scope(1);  // SLB0
    Assert.isNotNull(s, "SLB0 scope should exist");

    nodepool_header *h = get_nodepool_header(s);
    usize initial_capacity = h->capacity;

    printf("  Initial capacity: %zu bytes (from previous tests)\n", initial_capacity);
    printf("  Allocating mixed nodes in pattern: 10 page, 10 btree, repeat...\n");

    // Act: Interleave page_node and btree_node allocations
    // This stresses both allocation paths and their collision detection
    int cycles = 100;
    for (int cycle = 0; cycle < cycles; cycle++) {
        // Allocate page_nodes (grow up)
        for (int i = 0; i < 10; i++) {
            uint16_t idx = nodepool_alloc_page_node(s);
            Assert.isTrue(idx != PAGE_NODE_NULL, "Page allocation failed at cycle %d", cycle);
        }

        // Allocate btree_nodes (grow down)
        for (int i = 0; i < 10; i++) {
            node_idx idx = nodepool_alloc_btree_node(s);
            Assert.isTrue(idx != NODE_NULL, "BTree allocation failed at cycle %d", cycle);
        }

        // Print progress every 20 cycles
        if ((cycle + 1) % 20 == 0) {
            h = get_nodepool_header(s);
            printf("  After %d cycles: capacity=%zu bytes\n", cycle + 1, h->capacity);
        }
    }

    // Assert: Pool grew appropriately from mixed allocations
    h = get_nodepool_header(s);
    printf("  Final capacity: %zu bytes (mixed allocations)\n", h->capacity);
    Assert.isTrue(h->capacity > initial_capacity,
                  "Pool should have grown from mixed allocations: %zu -> %zu", initial_capacity,
                  h->capacity);
}
#endif

#if 1  // Region: Test Registration
__attribute__((constructor)) void init_nodepool_btree_growth_tests(void) {
    testset("NodePool: BTree Node Growth", set_config, set_teardown);

    testcase("NPB-01: Prevent btree_node exhaustion",
             test_btreenode_exhaustion_prevented_by_growth);
    testcase("NPB-02: Automatic btree_node mremap growth", test_btreenode_growth_via_mremap);
    testcase("NPB-03: Mixed page+btree node growth", test_mixed_pagenode_btreenode_growth);
}
#endif
