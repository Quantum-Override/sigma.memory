/*
 *  Test File: test_btree_page.c
 *  Description: Test per-page B-Tree operations for MTIS Tier 2
 *  Date: 2026-02-11
 *
 *  MTIS: Multi-Tiered Indexing Schema
 *  - Tier 1: Skip list (PageList) for O(log n) page lookup
 *  - Tier 2: Per-page B-trees for O(log n) block allocation within pages
 *
 *  Testing Philosophy:
 *  - Foundational tests in clean state (reset_nodepool() per test)
 *  - Test invariants: BST property, hint accuracy, coalescing
 *  - Build from simple (single node) to complex (multiple nodes)
 */

#include <sigtest/sigtest.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "internal/memory.h"
#include "internal/node_pool.h"
#include "memory.h"

#if 1  // Region: Test Set Setup & Teardown
static scope test_scope = NULL;

static void set_config(FILE **log_stream) {
    *log_stream = fopen("logs/test_btree_page.log", "w");

    // Warmup: Ensure memory system is initialized
    (void)Memory.state();

    // Create minimal test scope with NodePool
    test_scope = (scope)malloc(sizeof(sc_scope));
    memset(test_scope, 0, sizeof(sc_scope));

    test_scope->scope_id = 99;
    strcpy(test_scope->name, "btree_page_test");

    // Allocate NodePool (16KB for testing - room for page_nodes + btree_nodes)
    usize pool_size = 16384;
    test_scope->nodepool_base =
        (addr)mmap(NULL, pool_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (test_scope->nodepool_base == (addr)MAP_FAILED) {
        fprintf(stderr, "Failed to mmap NodePool\n");
        test_scope->nodepool_base = ADDR_EMPTY;
        return;
    }

    // Initialize NodePool header
    nodepool_header *header = (nodepool_header *)test_scope->nodepool_base;
    memset(header, 0, sizeof(nodepool_header));
    header->capacity = pool_size;
    header->page_count = 0;
    header->page_alloc_offset = sizeof(nodepool_header);  // Empty, ready for first alloc
    header->btree_alloc_offset = pool_size;               // Grows down from top
    header->skip_list_head = PAGE_NODE_NULL;
}

static void set_teardown(void) {
    if (test_scope) {
        if (test_scope->nodepool_base != ADDR_EMPTY) {
            munmap((void *)test_scope->nodepool_base, 16384);
        }
        free(test_scope);
        test_scope = NULL;
    }
}

// Helper to reset NodePool between tests
static void reset_nodepool(void) {
    if (!test_scope || test_scope->nodepool_base == ADDR_EMPTY) {
        return;
    }

    nodepool_header *header = (nodepool_header *)test_scope->nodepool_base;
    usize pool_size = header->capacity;

    // Clear entire NodePool
    memset((void *)test_scope->nodepool_base, 0, pool_size);

    // Reinitialize header
    header->capacity = pool_size;
    header->page_count = 0;
    header->page_alloc_offset = sizeof(nodepool_header);  // Empty, ready for first alloc
    header->btree_alloc_offset = pool_size;
    header->skip_list_head = PAGE_NODE_NULL;
}
#endif

#if 1  // Region: Helper Functions
static nodepool_header *get_header(void) {
    if (!test_scope || test_scope->nodepool_base == ADDR_EMPTY) {
        return NULL;
    }
    return (nodepool_header *)test_scope->nodepool_base;
}

// Allocate a page_node slot
static uint16_t alloc_page_node_slot(void) {
    nodepool_header *header = get_header();
    if (!header) return PAGE_NODE_NULL;

    usize offset = header->page_alloc_offset;
    usize next_offset = offset + sizeof(page_node);

    if (next_offset > header->btree_alloc_offset) {
        return PAGE_NODE_NULL;
    }

    // Calculate index (index 1 starts at offset=header, index 2 at offset=header+sizeof)
    usize idx = (offset - sizeof(nodepool_header)) / sizeof(page_node) + 1;
    header->page_alloc_offset = next_offset;

    page_node *node = (page_node *)(test_scope->nodepool_base + offset);
    memset(node, 0, sizeof(page_node));

    return (uint16_t)idx;
}

static page_node *get_page_node(uint16_t idx) {
    if (idx == PAGE_NODE_NULL || !test_scope || test_scope->nodepool_base == ADDR_EMPTY) {
        return NULL;
    }

    // Index 1 is at offset=header, index 2 at offset=header+sizeof(page_node), etc.
    usize offset = sizeof(nodepool_header) + ((idx - 1) * sizeof(page_node));
    return (page_node *)(test_scope->nodepool_base + offset);
}

// Allocate a btree_node slot (grows down from top)
static node_idx alloc_btree_node(void) {
    nodepool_header *header = get_header();
    if (!header) return NODE_NULL;

    usize new_offset = header->btree_alloc_offset - sizeof(sc_node);

    // Check collision with page_nodes growing up
    if (new_offset < header->page_alloc_offset) {
        return NODE_NULL;  // Pool exhausted
    }

    header->btree_alloc_offset = new_offset;

    // Calculate index from top (index 1 is first node from top)
    usize from_top = (header->capacity - new_offset) / sizeof(sc_node);

    // Zero the node
    sc_node *node = (sc_node *)(test_scope->nodepool_base + new_offset);
    memset(node, 0, sizeof(sc_node));

    return (node_idx)from_top;
}

static sc_node *get_btree_node(node_idx idx) {
    if (idx == NODE_NULL || !test_scope || test_scope->nodepool_base == ADDR_EMPTY) {
        return NULL;
    }

    nodepool_header *header = get_header();
    if (!header) return NULL;

    // Match nodepool_get_btree_node() calculation exactly
    addr base = test_scope->nodepool_base;
    usize capacity_nodes = header->capacity / sizeof(sc_node);
    usize offset = header->capacity - ((capacity_nodes - idx) * sizeof(sc_node));

    // Bounds check
    if (offset < header->btree_alloc_offset) {
        return NULL;
    }

    return (sc_node *)(base + offset);
}
#endif

#if 1  // Region: Warmup Test
// Warmup test to normalize timing for subsequent test cases
// This test absorbs first-call overhead (PLT/GOT resolution, cache warmup, etc.)
void test_btree_warmup(void) {
    reset_nodepool();

    uint16_t page_idx = alloc_page_node_slot();
    page_node *page = get_page_node(page_idx);
    page->page_base = 0x100000;
    page->btree_root = NODE_NULL;
    page->block_count = 0;

    // Simple insert to warm up all code paths
    addr alloc_start = 0x100000 + 1536;
    usize alloc_length = 64;
    node_idx out_idx = NODE_NULL;

    (void)btree_page_insert(test_scope, page_idx, alloc_start, alloc_length, &out_idx);
}
#endif

#if 1  // Region: B-Tree Insert Tests
void test_btree_insert_single_node(void) {
    reset_nodepool();

    // Setup: Create page in skip list
    uint16_t page_idx = alloc_page_node_slot();
    page_node *page = get_page_node(page_idx);
    page->page_base = 0x100000;
    page->btree_root = NODE_NULL;
    page->block_count = 0;

    // Insert single allocation
    addr alloc_start = 0x100000 + 1536;  // After page metadata
    usize alloc_length = 64;
    node_idx out_idx = NODE_NULL;

    int result = btree_page_insert(test_scope, page_idx, alloc_start, alloc_length, &out_idx);

    Assert.isTrue(result == OK, "Insert should succeed");
    Assert.isTrue(out_idx != NODE_NULL, "Should return valid node index");
    Assert.isTrue(page->btree_root == out_idx, "Root should point to new node");
    Assert.isTrue(page->block_count == 1, "Block count should be 1");

    // Verify node contents
    sc_node *node = get_btree_node(out_idx);
    Assert.isNotNull(node, "Node should be accessible");
    Assert.isTrue(node->start == alloc_start, "Node start should match");
    Assert.isTrue(node->length == alloc_length, "Node length should match");
    Assert.isTrue(node->left_idx == NODE_NULL, "Left child should be NULL");
    Assert.isTrue(node->right_idx == NODE_NULL, "Right child should be NULL");
    Assert.isTrue((node->info & NODE_FREE_FLAG) == 0, "Node should not be marked free");
}

void test_btree_insert_maintains_bst(void) {
    reset_nodepool();

    // Setup page
    uint16_t page_idx = alloc_page_node_slot();
    page_node *page = get_page_node(page_idx);
    page->page_base = 0x100000;
    page->btree_root = NODE_NULL;
    page->block_count = 0;

    // Insert 3 nodes with addresses out of order: 0x200000, 0x100000, 0x300000
    // Expected BST: 0x200000 (root), 0x100000 (left), 0x300000 (right)
    addr addrs[] = {0x200000, 0x100000, 0x300000};
    node_idx indices[3];

    for (int i = 0; i < 3; i++) {
        int result = btree_page_insert(test_scope, page_idx, addrs[i], 64, &indices[i]);
        Assert.isTrue(result == OK, "Insert %d should succeed", i);
        Assert.isTrue(indices[i] != NODE_NULL, "Should return valid node index");
    }

    // Verify tree structure
    sc_node *root = get_btree_node(page->btree_root);
    Assert.isNotNull(root, "Root should be accessible");
    Assert.isTrue(root->start == 0x200000, "Root should be 0x200000");

    // Verify left child
    Assert.isTrue(root->left_idx != NODE_NULL, "Root should have left child");
    sc_node *left = get_btree_node(root->left_idx);
    Assert.isNotNull(left, "Left child should be accessible");
    Assert.isTrue(left->start == 0x100000, "Left child should be 0x100000");
    Assert.isTrue(left->start < root->start, "BST violation: left >= root");

    // Verify right child
    Assert.isTrue(root->right_idx != NODE_NULL, "Root should have right child");
    sc_node *right = get_btree_node(root->right_idx);
    Assert.isNotNull(right, "Right child should be accessible");
    Assert.isTrue(right->start == 0x300000, "Right child should be 0x300000");
    Assert.isTrue(right->start > root->start, "BST violation: right <= root");

    // Verify block count
    Assert.isTrue(page->block_count == 3, "Block count should be 3");
}

void test_btree_insert_updates_count(void) {
    reset_nodepool();

    // Setup page
    uint16_t page_idx = alloc_page_node_slot();
    page_node *page = get_page_node(page_idx);
    page->page_base = 0x100000;
    page->btree_root = NODE_NULL;
    page->block_count = 0;

    // Insert 5 nodes
    const int COUNT = 5;
    for (int i = 0; i < COUNT; i++) {
        addr alloc_addr = 0x100000 + (i * 1000);
        int result = btree_page_insert(test_scope, page_idx, alloc_addr, 64, NULL);
        Assert.isTrue(result == OK, "Insert %d should succeed", i);
        Assert.isTrue(page->block_count == (usize)(i + 1), "Block count should be %d", i + 1);
    }

    Assert.isTrue(page->block_count == COUNT, "Final block count should be %d", COUNT);
}
#endif

#if 1  // Region: B-Tree Search Tests
void test_btree_search_exact_match(void) {
    reset_nodepool();

    // Setup page with 3 nodes
    uint16_t page_idx = alloc_page_node_slot();
    page_node *page = get_page_node(page_idx);
    page->page_base = 0x100000;
    page->btree_root = NODE_NULL;
    page->block_count = 0;

    addr addrs[] = {0x200000, 0x100000, 0x300000};
    node_idx inserted_indices[3];

    for (int i = 0; i < 3; i++) {
        btree_page_insert(test_scope, page_idx, addrs[i], 64, &inserted_indices[i]);
    }

    // Search for each inserted address
    for (int i = 0; i < 3; i++) {
        node_idx found_idx = NODE_NULL;
        int result = btree_page_search(test_scope, page_idx, addrs[i], &found_idx);

        Assert.isTrue(result == OK, "Search for 0x%lx should succeed", addrs[i]);
        Assert.isTrue(found_idx != NODE_NULL, "Should return valid node index");

        sc_node *found = get_btree_node(found_idx);
        Assert.isNotNull(found, "Found node should be accessible");
        Assert.isTrue(found->start == addrs[i], "Found node should match address 0x%lx", addrs[i]);
    }
}

void test_btree_search_not_found(void) {
    reset_nodepool();

    // Setup page with 3 nodes
    uint16_t page_idx = alloc_page_node_slot();
    page_node *page = get_page_node(page_idx);
    page->page_base = 0x100000;
    page->btree_root = NODE_NULL;
    page->block_count = 0;

    addr addrs[] = {0x200000, 0x100000, 0x300000};
    for (int i = 0; i < 3; i++) {
        btree_page_insert(test_scope, page_idx, addrs[i], 64, NULL);
    }

    // Search for addresses not in tree
    addr not_found_addrs[] = {0x50000, 0x150000, 0x250000, 0x400000};

    for (int i = 0; i < 4; i++) {
        node_idx found_idx = NODE_NULL;
        int result = btree_page_search(test_scope, page_idx, not_found_addrs[i], &found_idx);

        Assert.isTrue(result == ERR, "Search for 0x%lx should fail", not_found_addrs[i]);
    }
}

void test_btree_search_best_fit(void) {
    reset_nodepool();

    // Setup page with blocks of different sizes
    uint16_t page_idx = alloc_page_node_slot();
    page_node *page = get_page_node(page_idx);
    page->page_base = 0x100000;
    page->btree_root = NODE_NULL;
    page->block_count = 0;

    // Insert blocks: 32, 64, 128, 256 bytes
    node_idx idx_32, idx_64, idx_128, idx_256;
    btree_page_insert(test_scope, page_idx, 0x100000, 32, &idx_32);
    btree_page_insert(test_scope, page_idx, 0x200000, 64, &idx_64);
    btree_page_insert(test_scope, page_idx, 0x300000, 128, &idx_128);
    btree_page_insert(test_scope, page_idx, 0x400000, 256, &idx_256);

    // Mark 64 and 128 as free (32 and 256 are allocated)
    sc_node *node_64 = get_btree_node(idx_64);
    sc_node *node_128 = get_btree_node(idx_128);
    node_64->info |= NODE_FREE_FLAG;
    node_128->info |= NODE_FREE_FLAG;

    // Search for size=50 -> should return 64-byte block (smallest fit)
    node_idx found_idx = NODE_NULL;
    int result = btree_page_find_free(test_scope, page_idx, 50, &found_idx);

    Assert.isTrue(result == OK, "Should find free block");
    Assert.isTrue(found_idx == idx_64, "Should return 64-byte block for size 50");

    // Search for size=100 -> should return 128-byte block (only fit)
    found_idx = NODE_NULL;
    result = btree_page_find_free(test_scope, page_idx, 100, &found_idx);

    Assert.isTrue(result == OK, "Should find free block");
    Assert.isTrue(found_idx == idx_128, "Should return 128-byte block for size 100");

    // Search for size=300 -> should fail (no free block large enough)
    found_idx = NODE_NULL;
    result = btree_page_find_free(test_scope, page_idx, 300, &found_idx);

    Assert.isTrue(result == ERR, "Should fail when no block large enough");
}
#endif

#if 1  // Region: B-Tree Delete Tests
void test_btree_delete_leaf(void) {
    reset_nodepool();

    // Setup page with 3 nodes
    uint16_t page_idx = alloc_page_node_slot();
    page_node *page = get_page_node(page_idx);
    page->page_base = 0x100000;
    page->btree_root = NODE_NULL;
    page->block_count = 0;

    // Insert: 0x200000 (root), 0x100000 (left leaf), 0x300000 (right leaf)
    node_idx idx_root, idx_left, idx_right;
    btree_page_insert(test_scope, page_idx, 0x200000, 64, &idx_root);
    btree_page_insert(test_scope, page_idx, 0x100000, 64, &idx_left);
    btree_page_insert(test_scope, page_idx, 0x300000, 64, &idx_right);

    Assert.isTrue(page->block_count == 3, "Should have 3 blocks");

    // Delete left leaf (0x100000)
    int result = btree_page_delete(test_scope, page_idx, idx_left);

    Assert.isTrue(result == OK, "Delete should succeed");
    Assert.isTrue(page->block_count == 2, "Should have 2 blocks after delete");

    // Verify root still exists
    sc_node *root = get_btree_node(page->btree_root);
    Assert.isNotNull(root, "Root should still exist");
    Assert.isTrue(root->start == 0x200000, "Root should be unchanged");
    Assert.isTrue(root->left_idx == NODE_NULL, "Left child should be gone");
    Assert.isTrue(root->right_idx == idx_right, "Right child should remain");
}

void test_btree_delete_single_child(void) {
    reset_nodepool();

    // Setup page with nodes forming a chain
    uint16_t page_idx = alloc_page_node_slot();
    page_node *page = get_page_node(page_idx);
    page->page_base = 0x100000;
    page->btree_root = NODE_NULL;
    page->block_count = 0;

    // Insert: 0x200000 (root), 0x100000 (left), 0x50000 (left-left)
    node_idx idx_root, idx_left, idx_left_left;
    btree_page_insert(test_scope, page_idx, 0x200000, 64, &idx_root);
    btree_page_insert(test_scope, page_idx, 0x100000, 64, &idx_left);
    btree_page_insert(test_scope, page_idx, 0x50000, 64, &idx_left_left);

    // Delete middle node with single child (0x100000)
    int result = btree_page_delete(test_scope, page_idx, idx_left);

    Assert.isTrue(result == OK, "Delete should succeed");
    Assert.isTrue(page->block_count == 2, "Should have 2 blocks after delete");

    // Verify structure: root's left should now point to left-left
    sc_node *root = get_btree_node(page->btree_root);
    Assert.isNotNull(root, "Root should exist");
    Assert.isTrue(root->left_idx == idx_left_left, "Root's left should be 0x50000 node");

    sc_node *new_left = get_btree_node(root->left_idx);
    Assert.isNotNull(new_left, "New left child should exist");
    Assert.isTrue(new_left->start == 0x50000, "New left should be 0x50000");
}

void test_btree_delete_two_children(void) {
    reset_nodepool();

    // Setup page with full tree
    uint16_t page_idx = alloc_page_node_slot();
    page_node *page = get_page_node(page_idx);
    page->page_base = 0x100000;
    page->btree_root = NODE_NULL;
    page->block_count = 0;

    // Insert nodes to form complete tree
    // Root: 0x200000, Left: 0x100000, Right: 0x300000, Right-Left: 0x250000
    node_idx idx_root;
    btree_page_insert(test_scope, page_idx, 0x200000, 64, &idx_root);
    btree_page_insert(test_scope, page_idx, 0x100000, 64, NULL);
    btree_page_insert(test_scope, page_idx, 0x300000, 64, NULL);
    btree_page_insert(test_scope, page_idx, 0x250000, 64, NULL);

    // Delete root (has two children)
    int result = btree_page_delete(test_scope, page_idx, idx_root);

    Assert.isTrue(result == OK, "Delete should succeed");
    Assert.isTrue(page->block_count == 3, "Should have 3 blocks after delete");

    // Verify in-order successor (0x250000) replaced root
    sc_node *new_root = get_btree_node(page->btree_root);
    Assert.isNotNull(new_root, "New root should exist");
    Assert.isTrue(new_root->start == 0x250000, "Successor should replace root");

    // Verify BST property maintained
    Assert.isTrue(new_root->left_idx != NODE_NULL, "Should have left child");
    Assert.isTrue(new_root->right_idx != NODE_NULL, "Should have right child");

    sc_node *left = get_btree_node(new_root->left_idx);
    sc_node *right = get_btree_node(new_root->right_idx);
    Assert.isTrue(left->start < new_root->start, "BST: left < root");
    Assert.isTrue(right->start > new_root->start, "BST: right > root");
}

void test_btree_coalesce_left_neighbor(void) {
    reset_nodepool();

    // Setup page with three adjacent blocks: 0x100000 (64), 0x100040 (64), 0x100080 (64)
    uint16_t page_idx = alloc_page_node_slot();
    page_node *page = get_page_node(page_idx);
    page->page_base = 0x100000;
    page->btree_root = NODE_NULL;
    page->block_count = 0;

    node_idx idx1, idx2, idx3;
    btree_page_insert(test_scope, page_idx, 0x100000, 64, &idx1);
    btree_page_insert(test_scope, page_idx, 0x100040, 64, &idx2);
    btree_page_insert(test_scope, page_idx, 0x100080, 64, &idx3);

    // Mark first two as free
    sc_node *node1 = get_btree_node(idx1);
    sc_node *node2 = get_btree_node(idx2);
    node1->info |= NODE_FREE_FLAG;
    node2->info |= NODE_FREE_FLAG;

    // Coalesce middle block (should merge with left)
    int result = btree_page_coalesce(test_scope, page_idx, idx2);

    Assert.isTrue(result == OK, "Coalesce should succeed");
    Assert.isTrue(page->block_count == 2, "Should have 2 blocks after coalesce");

    // Verify merged block
    node1 = get_btree_node(idx1);  // Refresh pointer
    Assert.isTrue(node1->length == 128, "Merged block should be 128 bytes");
    Assert.isTrue(node1->start == 0x100000, "Merged block should start at 0x100000");
}

void test_btree_coalesce_right_neighbor(void) {
    reset_nodepool();

    // Setup page with three adjacent blocks
    uint16_t page_idx = alloc_page_node_slot();
    page_node *page = get_page_node(page_idx);
    page->page_base = 0x100000;
    page->btree_root = NODE_NULL;
    page->block_count = 0;

    node_idx idx1, idx2, idx3;
    btree_page_insert(test_scope, page_idx, 0x100000, 64, &idx1);
    btree_page_insert(test_scope, page_idx, 0x100040, 64, &idx2);
    btree_page_insert(test_scope, page_idx, 0x100080, 64, &idx3);

    // Mark middle and last as free
    sc_node *node2 = get_btree_node(idx2);
    sc_node *node3 = get_btree_node(idx3);
    node2->info |= NODE_FREE_FLAG;
    node3->info |= NODE_FREE_FLAG;

    // Coalesce middle block (should merge with right)
    int result = btree_page_coalesce(test_scope, page_idx, idx2);

    Assert.isTrue(result == OK, "Coalesce should succeed");
    Assert.isTrue(page->block_count == 2, "Should have 2 blocks after coalesce");

    // Verify merged block
    node2 = get_btree_node(idx2);  // Refresh pointer
    Assert.isTrue(node2->length == 128, "Merged block should be 128 bytes");
    Assert.isTrue(node2->start == 0x100040, "Merged block should start at 0x100040");
}

void test_btree_coalesce_both_neighbors(void) {
    reset_nodepool();

    // Setup page with three adjacent blocks
    uint16_t page_idx = alloc_page_node_slot();
    page_node *page = get_page_node(page_idx);
    page->page_base = 0x100000;
    page->btree_root = NODE_NULL;
    page->block_count = 0;

    node_idx idx1, idx2, idx3;
    btree_page_insert(test_scope, page_idx, 0x100000, 64, &idx1);
    btree_page_insert(test_scope, page_idx, 0x100040, 64, &idx2);
    btree_page_insert(test_scope, page_idx, 0x100080, 64, &idx3);

    // Mark all three as free
    sc_node *node1 = get_btree_node(idx1);
    sc_node *node2 = get_btree_node(idx2);
    sc_node *node3 = get_btree_node(idx3);
    node1->info |= NODE_FREE_FLAG;
    node2->info |= NODE_FREE_FLAG;
    node3->info |= NODE_FREE_FLAG;

    // Coalesce middle block (should merge with both)
    int result = btree_page_coalesce(test_scope, page_idx, idx2);

    Assert.isTrue(result == OK, "Coalesce should succeed");
    Assert.isTrue(page->block_count == 1, "Should have 1 block after coalesce");

    // Verify merged block
    node1 = get_btree_node(idx1);  // Refresh pointer
    Assert.isTrue(node1->length == 192, "Merged block should be 192 bytes");
    Assert.isTrue(node1->start == 0x100000, "Merged block should start at 0x100000");
}

void test_btree_coalesce_not_adjacent(void) {
    reset_nodepool();

    // Setup page with non-adjacent blocks: 0x100000 (64), 0x200000 (64)
    uint16_t page_idx = alloc_page_node_slot();
    page_node *page = get_page_node(page_idx);
    page->page_base = 0x100000;
    page->btree_root = NODE_NULL;
    page->block_count = 0;

    node_idx idx1, idx2;
    btree_page_insert(test_scope, page_idx, 0x100000, 64, &idx1);
    btree_page_insert(test_scope, page_idx, 0x200000, 64, &idx2);

    // Mark both as free
    sc_node *node1 = get_btree_node(idx1);
    sc_node *node2 = get_btree_node(idx2);
    node1->info |= NODE_FREE_FLAG;
    node2->info |= NODE_FREE_FLAG;

    // Try to coalesce (should fail - not adjacent)
    int result = btree_page_coalesce(test_scope, page_idx, idx1);

    Assert.isTrue(result == ERR, "Coalesce should fail for non-adjacent blocks");
    Assert.isTrue(page->block_count == 2, "Should still have 2 blocks");
}
#endif

#if 1  // Region: Test Registration
__attribute__((constructor)) void init_btree_page_tests(void) {
    testset("MTIS: Per-Page B-Tree Operations", set_config, set_teardown);

    // Warmup test (absorbs first-call overhead)
    testcase("B-Tree: warmup", test_btree_warmup);

    // Insert tests
    testcase("B-Tree: insert single node", test_btree_insert_single_node);
    testcase("B-Tree: insert maintains BST", test_btree_insert_maintains_bst);
    testcase("B-Tree: insert updates count", test_btree_insert_updates_count);

    // Search tests
    testcase("B-Tree: search exact match", test_btree_search_exact_match);
    testcase("B-Tree: search not found", test_btree_search_not_found);
    testcase("B-Tree: search best fit", test_btree_search_best_fit);

    // Delete tests
    testcase("B-Tree: delete leaf", test_btree_delete_leaf);
    testcase("B-Tree: delete single child", test_btree_delete_single_child);
    testcase("B-Tree: delete two children", test_btree_delete_two_children);

    // Coalesce tests
    testcase("B-Tree: coalesce left neighbor", test_btree_coalesce_left_neighbor);
    testcase("B-Tree: coalesce right neighbor", test_btree_coalesce_right_neighbor);
    testcase("B-Tree: coalesce both neighbors", test_btree_coalesce_both_neighbors);
    testcase("B-Tree: coalesce not adjacent", test_btree_coalesce_not_adjacent);
}
#endif
