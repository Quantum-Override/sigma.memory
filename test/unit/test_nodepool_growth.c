/*
 * SigmaCore
 * Copyright (c) 2025 David Boarman (BadKraft) and contributors
 * QuantumOverride [Q|]
 * ----------------------------------------------
 * MIT License
 * ----------------------------------------------
 * File: test_nodepool_growth.c
 * Description: Tests for NodePool automatic growth via mremap
 *              Testing NodePool page_node and btree_node growth
 */

#include <sigtest/sigtest.h>
#include "internal/memory.h"
#include "internal/node_pool.h"
#include "memory.h"

#if 1  // Region: Test Set Setup & Teardown
static void set_config(FILE **log_stream) {
    *log_stream = fopen("logs/test_nodepool_growth.log", "w");
    sbyte state = Memory.state();
    Assert.isTrue(state & MEM_STATE_READY, "Memory system should be ready");
}

static void set_teardown(void) {
    // Cleanup
}
#endif

#if 1  // Region: Helper Functions
// Helper to get nodepool header
static nodepool_header* get_nodepool_header(scope s) {
    if (s == NULL || s->nodepool_base == ADDR_EMPTY) {
        return NULL;
    }
    return (nodepool_header*)s->nodepool_base;
}
#endif

#if 1  // Region: NPP - NodePool Page Node Tests
// ============================================================================
// NPP-01: Test page_node exhaustion NO LONGER OCCURS (mremap prevents it)
// ============================================================================
void test_pagenode_exhaustion_returns_null(void) {
    // Arrange: Get SLB0 and calculate initial page_node capacity
    scope s = Memory.get_scope(1);  // SLB0
    Assert.isNotNull(s, "SLB0 scope should exist");
    
    nodepool_header *h = get_nodepool_header(s);
    Assert.isNotNull(h, "NodePool header should exist");
    
    // Record initial capacity
    usize initial_capacity = h->capacity;
    usize available_space = initial_capacity - sizeof(nodepool_header);
    uint16_t max_page_nodes = available_space / sizeof(page_node);
    
    printf("  Initial capacity: %zu bytes\n", initial_capacity);
    printf("  Max page_nodes (theoretical): %u\n", max_page_nodes);
    
    // Act: Allocate 2x theoretical capacity to force growth
    uint16_t target = max_page_nodes * 2;
    printf("  Attempting %u allocations (2x capacity)...\n", target);
    
    for (uint16_t i = 0; i < target; i++) {
        uint16_t idx = nodepool_alloc_page_node(s);
        Assert.isTrue(idx != PAGE_NODE_NULL, 
            "Allocation %u should succeed with mremap (got NULL)", i);
    }
    
    // Assert: Pool should have grown
    h = get_nodepool_header(s);
    printf("  Final capacity: %zu bytes (growth occurred)\n", h->capacity);
    Assert.isTrue(h->capacity > initial_capacity,
        "Capacity should have grown: %zu -> %zu", initial_capacity, h->capacity);
}

// ============================================================================
// NPP-02: Test page_node automatic growth via mremap (WILL FAIL)
// ============================================================================
void test_pagenode_growth_via_mremap(void) {
    // Arrange: Get SLB0 and initial capacity
    scope s = Memory.get_scope(1);  // SLB0
    Assert.isNotNull(s, "SLB0 scope should exist");
    
    nodepool_header *h = get_nodepool_header(s);
    usize initial_capacity = h->capacity;
    
    printf("  Initial capacity: %zu bytes\n", initial_capacity);
    
    // Calculate initial page_node capacity
    usize available_space = initial_capacity - sizeof(nodepool_header);
    uint16_t safe_allocation_count = (available_space / sizeof(page_node)) / 2;
    
    // Act: Allocate beyond initial capacity to trigger growth
    uint16_t target_allocations = safe_allocation_count + 100;
    printf("  Attempting %u allocations (safe=%u, forcing growth)...\n", 
           target_allocations, safe_allocation_count);
    
    for (uint16_t i = 0; i < target_allocations; i++) {
        uint16_t idx = nodepool_alloc_page_node(s);
        
        // This test expects mremap to succeed
        if (idx == PAGE_NODE_NULL) {
            Assert.fail("Allocation %u failed - mremap should have grown pool", i);
        }
    }
    
    // Assert: Capacity should have doubled
    h = get_nodepool_header(s);
    printf("  Final capacity: %zu bytes\n", h->capacity);
    
    Assert.isTrue(h->capacity >= initial_capacity * 2, 
        "Capacity should have doubled: %zu -> %zu", 
        initial_capacity, h->capacity);
}

// ============================================================================
// NPP-03: Test multiple growth cycles (adjustedfor test isolation)
// ============================================================================
void test_pagenode_multiple_growth_cycles(void) {
    // Arrange: Get SLB0 (may already be grown from previous tests)
    scope s = Memory.get_scope(1);  // SLB0
    Assert.isNotNull(s, "SLB0 scope should exist");
    
    nodepool_header *h = get_nodepool_header(s);
    usize initial_capacity = h->capacity;
    
    printf("  Initial capacity: %zu bytes (from previous tests)\n", initial_capacity);
    
    // Act: Allocate enough to trigger at least 1 more growth
    // Target: initial -> 2x initial
    usize target_capacity = initial_capacity * 2;
    uint16_t allocations_needed = (target_capacity - sizeof(nodepool_header)) / sizeof(page_node);
    
    // Be conservative - use 90% of theoretical capacity
    allocations_needed = (allocations_needed * 9) / 10;
    
    printf("  Attempting %u allocations to force 2x growth...\n", allocations_needed);
    
    usize last_capacity = initial_capacity;
    uint16_t growth_count = 0;
    
    for (uint16_t i = 0; i < allocations_needed; i++) {
        uint16_t idx = nodepool_alloc_page_node(s);
        
        if (idx == PAGE_NODE_NULL) {
            Assert.fail("Allocation %u failed - mremap should have grown pool", i);
        }
        
        // Check if capacity grew
        h = get_nodepool_header(s);
        if (h->capacity > last_capacity) {
            growth_count++;
            printf("  Growth #%u: %zu -> %zu bytes\n", 
                   growth_count, last_capacity, h->capacity);
            last_capacity = h->capacity;
        }
    }
    
    // Assert: Should have grown at least once
    h = get_nodepool_header(s);
    printf("  Final capacity: %zu bytes (%u growth cycles)\n", 
           h->capacity, growth_count);
    
    Assert.isTrue(h->capacity >= target_capacity,
        "Capacity should reach at least 2x initial: got %zu >= %zu", 
        h->capacity, target_capacity);
    Assert.isTrue(growth_count >= 1,
        "Should have grown at least 1 time, got %u", growth_count);
}
#endif

#if 1  // Region: Test Registration
__attribute__((constructor)) void init_nodepool_growth_tests(void) 
{
    testset("NodePool:Growth prevents exhaustion_teardown", set_config, set_teardown);

    testcase("NPP-01: Detect page_node exhaustion (baseline)", 
             test_pagenode_exhaustion_returns_null);
    testcase("NPP-02: Automatic growth via mremap", 
             test_pagenode_growth_via_mremap);  
    testcase("NPP-03: Multiple growth cycles", 
             test_pagenode_multiple_growth_cycles);
}
#endif
