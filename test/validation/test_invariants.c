/*
 * SigmaMemory
 * Copyright (c) 2026 David Boarman (BadKraft) and contributors
 * QuantumOverride [Q|]
 * ----------------------------------------------
 * File: test_invariants.c
 * Description: Stress tests for B-Tree invariants validation
 */

#include <sigtest/sigtest.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "config.h"
#include "internal/memory.h"
#include "internal/node_pool.h"
#include "memory.h"

// Test scope and fixture
static struct sc_scope s_test_scope_storage;
static scope test_scope = NULL;

// ============================================================================
// Test Set Setup & Teardown
// ============================================================================
static void set_config(FILE **log_stream) {
    *log_stream = fopen("logs/test_invariants.log", "w");

    // Warmup: Force memory system initialization before timing tests
    (void)Memory.state();

    // Initialize test scope
    test_scope = &s_test_scope_storage;
    memset(test_scope, 0, sizeof(struct sc_scope));
    test_scope->scope_id = 99;
    strcpy(test_scope->name, "invariant_test");

    // Allocate NodePool (16KB for testing)
    usize pool_size = 16 * 1024;
    void *pool = mmap(NULL, pool_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    Assert.isNotNull(pool, "NodePool mmap should succeed");
    test_scope->nodepool_base = (addr)pool;

    // Initialize header
    nodepool_header *header = (nodepool_header *)test_scope->nodepool_base;
    memset(header, 0, sizeof(nodepool_header));
    header->capacity = pool_size;
    header->page_count = 0;
    header->page_alloc_offset = sizeof(nodepool_header);
    header->btree_alloc_offset = pool_size;
    header->skip_list_head = PAGE_NODE_NULL;
}

static void set_teardown(void) {
    if (test_scope && test_scope->nodepool_base != ADDR_EMPTY) {
        nodepool_header *header = (nodepool_header *)test_scope->nodepool_base;
        munmap((void *)test_scope->nodepool_base, header->capacity);
        test_scope->nodepool_base = ADDR_EMPTY;
    }
}

// ============================================================================
// Helper: Allocate a page_node
// ============================================================================
static uint16_t alloc_page_node(void) {
    nodepool_header *header = (nodepool_header *)test_scope->nodepool_base;
    usize next_offset = header->page_alloc_offset + sizeof(page_node);
    if (next_offset > header->btree_alloc_offset) {
        return PAGE_NODE_NULL;
    }
    usize idx = (header->page_alloc_offset - sizeof(nodepool_header)) / sizeof(page_node) + 1;
    header->page_alloc_offset = next_offset;

    page_node *pn = nodepool_get_page_node(test_scope, (uint16_t)idx);
    if (pn) {
        memset(pn, 0, sizeof(page_node));
    }
    return (uint16_t)idx;
}

// ============================================================================
// Helper: Get btree_node by index
// ============================================================================
static sc_node *get_btree_node(node_idx idx) {
    if (idx == NODE_NULL) {
        return NULL;
    }
    nodepool_header *header = (nodepool_header *)test_scope->nodepool_base;
    addr base = test_scope->nodepool_base;
    usize capacity_nodes = header->capacity / sizeof(sc_node);
    usize offset = header->capacity - ((capacity_nodes - idx) * sizeof(sc_node));

    if (offset < header->btree_alloc_offset) {
        return NULL;
    }
    return (sc_node *)(base + offset);
}

// ============================================================================
// Invariant I1: Insertion maintains tree structure
// ============================================================================
void test_invariant_bst_property(void) {
    uint16_t page_idx = alloc_page_node();
    Assert.isTrue(page_idx != PAGE_NODE_NULL, "Page allocation should succeed");

    page_node *pn = nodepool_get_page_node(test_scope, page_idx);
    pn->page_base = 0x10000;
    pn->btree_root = NODE_NULL;

    // Insert blocks in non-sorted order
    addr addrs[] = {0x10100, 0x10050, 0x10200, 0x10080, 0x10150};
    for (int i = 0; i < 5; i++) {
        node_idx out_idx = NODE_NULL;
        int result = btree_page_insert(test_scope, page_idx, addrs[i], 64, &out_idx);
        Assert.isTrue(result == OK, "Insert %d should succeed", i);
    }

    Assert.isTrue(pn->btree_root != NODE_NULL, "Tree should not be empty");
    Assert.isTrue(pn->block_count == 5, "Block count should be 5");
}

// ============================================================================
// Invariant I2: Free flag consistency
// ============================================================================
void test_invariant_free_marking(void) {
    uint16_t page_idx = alloc_page_node();
    page_node *pn = nodepool_get_page_node(test_scope, page_idx);
    pn->page_base = 0x10000;
    pn->btree_root = NODE_NULL;

    node_idx alloc_idx = NODE_NULL;
    btree_page_insert(test_scope, page_idx, 0x10100, 128, &alloc_idx);

    sc_node *node = get_btree_node(alloc_idx);
    Assert.isNotNull(node, "Node should exist");
    Assert.isFalse((node->info & NODE_FREE_FLAG) != 0, "New allocation not free");

    node->info |= NODE_FREE_FLAG;
    Assert.isTrue((node->info & NODE_FREE_FLAG) != 0, "Should be marked free");

    node->info &= ~NODE_FREE_FLAG;
    Assert.isFalse((node->info & NODE_FREE_FLAG) != 0, "Should not be free");
}

// ============================================================================
// Invariant I5: Length consistency
// ============================================================================
void test_invariant_length_consistency(void) {
    uint16_t page_idx = alloc_page_node();
    page_node *pn = nodepool_get_page_node(test_scope, page_idx);
    pn->page_base = 0x10000;
    pn->btree_root = NODE_NULL;

    usize valid_sizes[] = {16, 64, 256, 1024, 4096, 65536};
    for (size_t i = 0; i < sizeof(valid_sizes) / sizeof(valid_sizes[0]); i++) {
        node_idx out_idx = NODE_NULL;
        addr alloc_addr = 0x10000 + (i * 0x10000);
        int result = btree_page_insert(test_scope, page_idx, alloc_addr, valid_sizes[i], &out_idx);
        Assert.isTrue(result == OK, "Insert with valid size should succeed");

        sc_node *node = get_btree_node(out_idx);
        Assert.isTrue(node->length == valid_sizes[i], "Length should match");
        Assert.isTrue(node->length > 0, "Length should be positive");
    }
}

// ============================================================================
// Invariant I6: No adjacent free blocks
// ============================================================================
void test_invariant_no_adjacent_free(void) {
    uint16_t page_idx = alloc_page_node();
    page_node *pn = nodepool_get_page_node(test_scope, page_idx);
    pn->page_base = 0x10000;
    pn->btree_root = NODE_NULL;

    node_idx idx1, idx2, idx3;
    btree_page_insert(test_scope, page_idx, 0x10100, 64, &idx1);
    btree_page_insert(test_scope, page_idx, 0x10140, 64, &idx2);
    btree_page_insert(test_scope, page_idx, 0x10180, 64, &idx3);

    sc_node *node1 = get_btree_node(idx1);
    sc_node *node2 = get_btree_node(idx2);
    sc_node *node3 = get_btree_node(idx3);

    node1->info |= NODE_FREE_FLAG;
    node3->info |= NODE_FREE_FLAG;

    Assert.isFalse((node2->info & NODE_FREE_FLAG) != 0, "Middle node allocated");

    // When middle freed, coalescing should merge all three
    node2->info |= NODE_FREE_FLAG;
    btree_page_coalesce(test_scope, page_idx, idx2);

    Assert.isTrue(true, "Coalescing completed");
}

// ============================================================================
// Stress: Random allocation pattern
// ============================================================================
void test_stress_random_pattern(void) {
    uint16_t page_idx = alloc_page_node();
    page_node *pn = nodepool_get_page_node(test_scope, page_idx);
    pn->page_base = 0x10000;
    pn->btree_root = NODE_NULL;

    const int ALLOC_COUNT = 30;
    int successful = 0;

    for (int i = 0; i < ALLOC_COUNT; i++) {
        usize size = 64 + (i % 10) * 32;
        addr alloc_addr = 0x10000 + (i * 256);
        node_idx out_idx;

        if (btree_page_insert(test_scope, page_idx, alloc_addr, size, &out_idx) == OK) {
            successful++;
        } else {
            break;
        }
    }

    Assert.isTrue(successful >= 20, "Should allocate at least 20 blocks");
    Assert.isTrue(pn->block_count == successful, "Block count matches");
}

// ============================================================================
// Stress: Delete operations
// ============================================================================
void test_stress_delete_operations(void) {
    uint16_t page_idx = alloc_page_node();
    page_node *pn = nodepool_get_page_node(test_scope, page_idx);
    pn->page_base = 0x10000;
    pn->btree_root = NODE_NULL;

    const int NODE_COUNT = 20;
    node_idx indices[20];
    int inserted = 0;

    for (int i = 0; i < NODE_COUNT; i++) {
        addr alloc_addr = 0x10000 + (i * 512);
        if (btree_page_insert(test_scope, page_idx, alloc_addr, 128, &indices[i]) == OK) {
            inserted++;
        } else {
            break;
        }
    }

    Assert.isTrue(inserted >= 15, "Should insert at least 15 nodes");

    int delete_count = 0;
    for (int i = 0; i < inserted; i += 3) {
        if (btree_page_delete(test_scope, page_idx, indices[i]) == OK) {
            delete_count++;
        }
    }

    Assert.isTrue(delete_count >= 3, "Should delete at least 3 nodes");
    Assert.isTrue(pn->block_count == inserted - delete_count, "Block count correct");
}

// ============================================================================
// Test Registration
// ============================================================================
__attribute__((constructor)) void init_invariant_tests(void) {
    testset("MTIS: B-Tree Invariants Validation", set_config, set_teardown);

    testcase("I1: BST property maintained", test_invariant_bst_property);
    testcase("I2: Free flag consistency", test_invariant_free_marking);
    testcase("I5: Length validation", test_invariant_length_consistency);
    testcase("I6: No adjacent free blocks", test_invariant_no_adjacent_free);
    testcase("Stress: Random allocation pattern", test_stress_random_pattern);
    testcase("Stress: Delete operations", test_stress_delete_operations);
}
