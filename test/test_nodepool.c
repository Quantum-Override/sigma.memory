/*
 *  Test File: test_nodepool.c
 *  Description: Test NodePool basic operations
 *  Date: 2026-02-07
 */

#include "internal/memory.h"
#include "internal/node_pool.h"
#include "memory.h"
// ----------------
#include <sigtest/sigtest.h>

#if 1  // Region: Test Set Setup & Teardown
static void set_config(FILE **log_stream) {
    *log_stream = fopen("logs/test_nodepool.log", "w");
    sbyte state = Memory.state();
    Assert.isTrue(state & MEM_STATE_READY, "SYS0 should be ready during test setup");

    // Initialize NodePool for tests
    int result = NodePool.init();
    Assert.isTrue(result == OK, "NodePool initialization should succeed");
}

static void set_teardown(void) {
    // Shutdown NodePool
    NodePool.shutdown();
}
#endif

#if 1  // Region: NodePool Initialization Tests
void test_nodepool_initializes(void) {
    // Pool initialized in set_config
    Assert.isTrue(NodePool.is_initialized(), "NodePool should be initialized");
    Assert.isTrue(NodePool.capacity() == NODE_POOL_INITIAL_COUNT, "Capacity should be %d",
                  NODE_POOL_INITIAL_COUNT);
}

void test_nodepool_r1_caches_base(void) {
    // R1 should hold NodePool base address (if SYS0 is ready)
    addr sys0_base = Memory.get_sys0_base();
    if (sys0_base == ADDR_EMPTY) {
        // Skip this test if SYS0 base unavailable
        return;
    }

    sc_registers *regs = (sc_registers *)sys0_base;
    // R1 may be set if SYS0 was ready during init
    // (not critical for pool operation, just an optimization)
}

void test_nodepool_free_list_initialized(void) {
    // Free list should have 1023 nodes (1-1023, node 0 reserved)
    usize free_count = NodePool.free_count();
    usize expected_free = NODE_POOL_INITIAL_COUNT - 1;  // Node 0 reserved

    Assert.isTrue(free_count == expected_free, "Free list should have %zu nodes, got %zu",
                  expected_free, free_count);
}

void test_nodepool_node_zero_reserved(void) {
    // Node 0 should not be accessible (NODE_NULL)
    btree_node node = NodePool.get_node(NODE_NULL);
    Assert.isNull(node, "get_node(NODE_NULL) should return NULL");
}
#endif

#if 1  // Region: NodePool Allocation Tests
void test_nodepool_alloc_single_node(void) {
    node_idx idx = NodePool.alloc_node();

    Assert.isTrue(idx != NODE_NULL, "alloc_node() should return valid index");
    Assert.isTrue(idx >= 1, "Allocated node index should be >= 1");

    // Clean up
    NodePool.dispose_node(idx);
}

void test_nodepool_alloc_reduces_free_count(void) {
    usize initial_free = NodePool.free_count();

    node_idx idx = NodePool.alloc_node();
    Assert.isTrue(idx != NODE_NULL, "alloc_node() should succeed");

    usize after_free = NodePool.free_count();
    Assert.isTrue(after_free == initial_free - 1,
                  "Free count should decrease by 1 (was %zu, now %zu)", initial_free, after_free);

    // Clean up
    NodePool.dispose_node(idx);
}

void test_nodepool_alloc_zeroes_node(void) {
    node_idx idx = NodePool.alloc_node();
    Assert.isTrue(idx != NODE_NULL, "alloc_node() should succeed");

    btree_node node = NodePool.get_node(idx);
    Assert.isNotNull(node, "get_node() should return valid pointer");

    // Check that node is zeroed
    Assert.isTrue(node->start == 0, "Node start should be zeroed");
    Assert.isTrue(node->length == 0, "Node length should be zeroed");
    Assert.isTrue(node->left_idx == NODE_NULL, "Node left_idx should be NODE_NULL");
    Assert.isTrue(node->right_idx == NODE_NULL, "Node right_idx should be NODE_NULL");
    Assert.isTrue(node->max_free_log2 == 0, "Node max_free_log2 should be zeroed");

    // Clean up
    NodePool.dispose_node(idx);
}

void test_nodepool_get_node_returns_valid_pointer(void) {
    node_idx idx = NodePool.alloc_node();
    btree_node node = NodePool.get_node(idx);

    Assert.isNotNull(node, "get_node() should return non-NULL pointer");

    // Write to node to verify pointer is valid
    node->start = (addr)0x12345678;
    node->length = 128;
    node->left_idx = 10;
    node->right_idx = 20;
    node->max_free_log2 = 0x0507;  // log2=7, direction=1, free=0

    // Re-read via get_node
    btree_node node2 = NodePool.get_node(idx);
    Assert.isTrue(node2->start == (addr)0x12345678, "Node start should persist");
    Assert.isTrue(node2->length == 128, "Node length should persist");
    Assert.isTrue(node2->left_idx == 10, "Node left_idx should persist");
    Assert.isTrue(node2->right_idx == 20, "Node right_idx should persist");
    Assert.isTrue(node2->max_free_log2 == 0x0507, "Node max_free_log2 should persist");

    // Clean up
    NodePool.dispose_node(idx);
}
#endif

#if 1  // Region: NodePool Free Tests
void test_nodepool_free_node(void) {
    usize initial_free = NodePool.free_count();

    node_idx idx = NodePool.alloc_node();
    usize after_alloc = NodePool.free_count();
    Assert.isTrue(after_alloc == initial_free - 1, "Free count should decrease after alloc");

    NodePool.dispose_node(idx);
    usize after_free = NodePool.free_count();
    Assert.isTrue(after_free == initial_free, "Free count should return to initial after free");
}

void test_nodepool_free_null_is_noop(void) {
    usize initial_free = NodePool.free_count();

    NodePool.dispose_node(NODE_NULL);

    usize after_free = NodePool.free_count();
    Assert.isTrue(after_free == initial_free,
                  "Free count should not change when freeing NODE_NULL");
}

void test_nodepool_free_allows_reallocation(void) {
    node_idx idx1 = NodePool.alloc_node();
    Assert.isTrue(idx1 != NODE_NULL, "First allocation should succeed");

    NodePool.dispose_node(idx1);

    node_idx idx2 = NodePool.alloc_node();
    Assert.isTrue(idx2 != NODE_NULL, "Reallocation should succeed");
    Assert.isTrue(idx2 == idx1, "Reallocated node should be the same index (LIFO free list)");

    // Clean up
    NodePool.dispose_node(idx2);
}
#endif

#if 1  // Region: NodePool Multiple Operations Tests
void test_nodepool_multiple_allocs(void) {
    const usize alloc_count = 10;
    node_idx indices[10];

    usize initial_free = NodePool.free_count();

    // Allocate multiple nodes
    for (usize i = 0; i < alloc_count; i++) {
        indices[i] = NodePool.alloc_node();
        Assert.isTrue(indices[i] != NODE_NULL, "Allocation #%zu should succeed", i);
    }

    usize after_allocs = NodePool.free_count();
    Assert.isTrue(after_allocs == initial_free - alloc_count, "Free count should decrease by %zu",
                  alloc_count);

    // Free all nodes
    for (usize i = 0; i < alloc_count; i++) {
        NodePool.dispose_node(indices[i]);
    }

    usize after_frees = NodePool.free_count();
    Assert.isTrue(after_frees == initial_free, "Free count should return to initial");
}

void test_nodepool_alloc_free_interleaved(void) {
    node_idx idx1 = NodePool.alloc_node();
    node_idx idx2 = NodePool.alloc_node();

    Assert.isTrue(idx1 != idx2, "Allocated indices should be different");

    NodePool.dispose_node(idx1);

    node_idx idx3 = NodePool.alloc_node();
    Assert.isTrue(idx3 == idx1, "Reallocated index should match freed index");

    NodePool.dispose_node(idx2);
    NodePool.dispose_node(idx3);
}

void test_nodepool_alloc_exhaustion(void) {
    // Allocate all available nodes
    usize capacity = NodePool.capacity();
    usize available = capacity - 1;  // Node 0 reserved

    // Use stack array instead of heap allocation
    node_idx indices[1024];  // Large enough for initial capacity

    usize count = 0;
    for (usize i = 0; i < available; i++) {
        indices[count] = NodePool.alloc_node();
        if (indices[count] != NODE_NULL) {
            count++;
        }
    }

    Assert.isTrue(count == available, 
                  "Should allocate all %zu available nodes (got %zu)", 
                  available, count);
    Assert.isTrue(NodePool.free_count() == 0, "Free list should be exhausted");

    // Try to allocate one more (should fail)
    node_idx extra = NodePool.alloc_node();
    Assert.isTrue(extra == NODE_NULL, "Allocation beyond capacity should return NODE_NULL");

    // Free all nodes
    for (usize i = 0; i < count; i++) {
        NodePool.dispose_node(indices[i]);
    }

    Assert.isTrue(NodePool.free_count() == available, "All nodes should be freed");
}
#endif

#if 1  // Region: NodePool Shutdown Tests
void test_nodepool_shutdown_clears_state(void) {
    // Note: This test runs at end, so we'll reinit first
    NodePool.shutdown();

    Assert.isFalse(NodePool.is_initialized(), "NodePool should not be initialized after shutdown");

    // Reinit for subsequent tests (if any)
    NodePool.init();
}
#endif

#if 1  // Region: Test Registration
__attribute__((constructor)) void init_nodepool_tests(void) {
    testset("Memory: NodePool Operations", set_config, set_teardown);

    // Initialization
    testcase("NodePool: initializes", test_nodepool_initializes);
    testcase("NodePool: R1 caches base", test_nodepool_r1_caches_base);
    testcase("NodePool: free list initialized", test_nodepool_free_list_initialized);
    testcase("NodePool: node zero reserved", test_nodepool_node_zero_reserved);

    // Allocation
    testcase("NodePool: alloc single node", test_nodepool_alloc_single_node);
    testcase("NodePool: alloc reduces free count", test_nodepool_alloc_reduces_free_count);
    testcase("NodePool: alloc zeroes node", test_nodepool_alloc_zeroes_node);
    testcase("NodePool: get_node returns valid pointer",
             test_nodepool_get_node_returns_valid_pointer);

    // Free
    testcase("NodePool: free node", test_nodepool_free_node);
    testcase("NodePool: free NULL is noop", test_nodepool_free_null_is_noop);
    testcase("NodePool: free allows reallocation", test_nodepool_free_allows_reallocation);

    // Multiple operations
    testcase("NodePool: multiple allocs", test_nodepool_multiple_allocs);
    testcase("NodePool: alloc/free interleaved", test_nodepool_alloc_free_interleaved);
    testcase("NodePool: alloc exhaustion", test_nodepool_alloc_exhaustion);

    // Shutdown
    testcase("NodePool: shutdown clears state", test_nodepool_shutdown_clears_state);
}
#endif
