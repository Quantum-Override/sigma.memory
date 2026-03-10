/*
 *  Test File: test_pagelist.c
 *  Description: Test PageList (skip list) operations for MTIS architecture
 *  Date: 2026-02-11
 *
 *  MTIS: Multi-Tiered Indexing Schema
 *  - Tier 1: Skip list (PageList) for O(log n) page lookup
 *  - Tier 2: Per-page B-trees for O(log n) block lookup
 */

#include <sigma.test/sigtest.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "internal/memory.h"
#include "internal/node_pool.h"
#include "memory.h"

#if 1  // Region: Test Set Setup & Teardown
static scope test_scope = NULL;

static void set_config(FILE **log_stream) {
    *log_stream = fopen("logs/test_pagelist.log", "w");

    // Warmup: Force memory system initialization before timing tests
    (void)Memory.state();

    // Create minimal test scope with NodePool
    test_scope = (scope)malloc(sizeof(sc_scope));
    memset(test_scope, 0, sizeof(sc_scope));

    test_scope->scope_id = 99;
    strcpy(test_scope->name, "pagelist_test");

    // Allocate NodePool (1 page = 8KB for testing)
    usize pool_size = 8192;
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
    // Reserve slot 0 by starting at slot 1
    header->page_alloc_offset = sizeof(nodepool_header) + sizeof(page_node);
    header->btree_alloc_offset = pool_size;
    header->skip_list_head = PAGE_NODE_NULL;
}

static void set_teardown(void) {
    if (test_scope) {
        if (test_scope->nodepool_base != ADDR_EMPTY) {
            munmap((void *)test_scope->nodepool_base, 8192);
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
    usize pool_size = header->capacity;  // Save capacity

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
// Get NodePool header for test scope
static nodepool_header *get_header(void) {
    if (!test_scope || test_scope->nodepool_base == ADDR_EMPTY) {
        return NULL;
    }
    return (nodepool_header *)test_scope->nodepool_base;
}

// Allocate a page_node slot (manual allocation for testing)
static uint16_t alloc_page_node_slot(void) {
    nodepool_header *header = get_header();
    if (!header) return PAGE_NODE_NULL;

    usize offset = header->page_alloc_offset;
    usize next_offset = offset + sizeof(page_node);

    // Check bounds
    if (next_offset > header->btree_alloc_offset) {
        return PAGE_NODE_NULL;
    }

    // Calculate index (index 1 starts at offset=header, index 2 at offset=header+sizeof)
    usize idx = (offset - sizeof(nodepool_header)) / sizeof(page_node) + 1;

    // Advance offset
    header->page_alloc_offset = next_offset;

    // Zero the page_node
    page_node *node = (page_node *)(test_scope->nodepool_base + offset);
    memset(node, 0, sizeof(page_node));

    return (uint16_t)idx;
}

// Get page_node by index
static page_node *get_page_node(uint16_t idx) {
    if (idx == PAGE_NODE_NULL || !test_scope || test_scope->nodepool_base == ADDR_EMPTY) {
        return NULL;
    }

    // Index 1 is at offset=header, index 2 at offset=header+sizeof(page_node), etc.
    usize offset = sizeof(nodepool_header) + ((idx - 1) * sizeof(page_node));
    return (page_node *)(test_scope->nodepool_base + offset);
}
#endif

#if 1  // Region: PageList Insert Tests
void test_pagelist_insert_single(void) {
    reset_nodepool();
    nodepool_header *header = get_header();
    Assert.isNotNull(header, "Header should be valid");

    // Allocate page_node slot
    uint16_t page_idx = alloc_page_node_slot();
    Assert.isTrue(page_idx != PAGE_NODE_NULL, "Should allocate page_node slot");

    // Set page base address
    addr page_base = (addr)0x100000;
    page_node *node = get_page_node(page_idx);
    node->page_base = page_base;

    // Insert into skip list
    int result = skiplist_insert(test_scope, page_base, page_idx);

    Assert.isTrue(result == OK, "skiplist_insert should return OK");
    Assert.isTrue(header->skip_list_head == page_idx, "Page should become head");
    Assert.isTrue(header->page_count == 1, "Page count should be 1");
    Assert.isTrue(node->page_base == page_base, "Page base should be set");
}

void test_pagelist_insert_maintains_order(void) {
    reset_nodepool();
    nodepool_header *header = get_header();

    // Insert three pages: 0x200000, 0x100000, 0x300000
    // Expected order after: 0x100000 -> 0x200000 -> 0x300000

    addr addrs[] = {0x200000, 0x100000, 0x300000};
    uint16_t indices[3];

    for (int i = 0; i < 3; i++) {
        indices[i] = alloc_page_node_slot();
        page_node *node = get_page_node(indices[i]);
        node->page_base = addrs[i];

        int result = skiplist_insert(test_scope, addrs[i], indices[i]);
        Assert.isTrue(result == OK, "Insert %d should succeed", i);
    }

    Assert.isTrue(header->page_count == 3, "Should have 3 pages");

    // Verify ordering by traversing level 0
    uint16_t current = header->skip_list_head;
    addr prev_addr = 0;
    int count = 0;

    while (current != PAGE_NODE_NULL && count < 10) {
        page_node *node = get_page_node(current);
        Assert.isNotNull(node, "Node should be valid");
        Assert.isTrue(node->page_base > prev_addr, "Addresses should be ascending");

        prev_addr = node->page_base;
        current = node->forward[0];
        count++;
    }

    Assert.isTrue(count == 3, "Should traverse 3 nodes");
}

void test_pagelist_insert_multiple(void) {
    reset_nodepool();
    nodepool_header *header = get_header();
    const int COUNT = 5;

    for (int i = 0; i < COUNT; i++) {
        uint16_t idx = alloc_page_node_slot();
        addr page_base = 0x100000 + (i * 0x100000);

        page_node *node = get_page_node(idx);
        node->page_base = page_base;

        int result = skiplist_insert(test_scope, page_base, idx);
        Assert.isTrue(result == OK, "Insert should succeed");
    }

    Assert.isTrue(header->page_count == (usize)COUNT, "Page count should be %d", COUNT);
}
#endif

#if 1  // Region: PageList Find Tests
void test_pagelist_find_containing(void) {
    // Insert page at 0x100000 (8KB page covers 0x100000 - 0x102000)
    uint16_t page_idx = alloc_page_node_slot();
    addr page_base = 0x100000;

    page_node *node = get_page_node(page_idx);
    node->page_base = page_base;

    skiplist_insert(test_scope, page_base, page_idx);

    // Search for address within page
    uint16_t found_idx = PAGE_NODE_NULL;
    addr search_addr = 0x100800;  // Middle of page

    int result = skiplist_find_containing(test_scope, search_addr, &found_idx);

    Assert.isTrue(result == OK, "Should find containing page");
    Assert.isTrue(found_idx == page_idx, "Should return correct page index");
}

void test_pagelist_find_containing_boundary(void) {
    uint16_t page_idx = alloc_page_node_slot();
    addr page_base = 0x100000;

    page_node *node = get_page_node(page_idx);
    node->page_base = page_base;
    skiplist_insert(test_scope, page_base, page_idx);

    // Test exact base address (should be included)
    uint16_t found_idx = PAGE_NODE_NULL;
    int result = skiplist_find_containing(test_scope, page_base, &found_idx);

    Assert.isTrue(result == OK, "Should find page at base address");
    Assert.isTrue(found_idx == page_idx, "Should return correct page");
}

void test_pagelist_find_containing_not_found(void) {
    // Insert page at 0x100000
    uint16_t page_idx = alloc_page_node_slot();
    page_node *node = get_page_node(page_idx);
    node->page_base = 0x100000;
    skiplist_insert(test_scope, 0x100000, page_idx);

    // Search for address before first page
    uint16_t found_idx = PAGE_NODE_NULL;
    int result = skiplist_find_containing(test_scope, 0x50000, &found_idx);

    Assert.isTrue(result == ERR, "Should not find page");
}

void test_pagelist_find_for_size(void) {
    reset_nodepool();
    // Insert page with some block_count
    uint16_t page_idx = alloc_page_node_slot();
    page_node *node = get_page_node(page_idx);
    node->page_base = 0x100000;
    node->block_count = 10;  // Has space
    skiplist_insert(test_scope, 0x100000, page_idx);

    // Find page with space
    uint16_t found_idx = PAGE_NODE_NULL;
    int result = skiplist_find_for_size(test_scope, 64, &found_idx);

    Assert.isTrue(result == OK, "Should find page with space");
    Assert.isTrue(found_idx == page_idx, "Should return correct page");
}
#endif

#if 1  // Region: PageList Remove Tests
void test_pagelist_remove_single(void) {
    reset_nodepool();
    nodepool_header *header = get_header();

    // Insert single page
    uint16_t page_idx = alloc_page_node_slot();
    page_node *node = get_page_node(page_idx);
    node->page_base = 0x100000;
    skiplist_insert(test_scope, 0x100000, page_idx);

    Assert.isTrue(header->page_count == 1, "Should have 1 page");

    // Remove it
    int result = skiplist_remove(test_scope, page_idx);

    Assert.isTrue(result == OK, "Remove should succeed");
    Assert.isTrue(header->skip_list_head == PAGE_NODE_NULL, "List should be empty");
    Assert.isTrue(header->page_count == 0, "Page count should be 0");
}

void test_pagelist_remove_head(void) {
    reset_nodepool();
    nodepool_header *header = get_header();

    // Insert two pages
    uint16_t idx1 = alloc_page_node_slot();
    uint16_t idx2 = alloc_page_node_slot();

    page_node *node1 = get_page_node(idx1);
    page_node *node2 = get_page_node(idx2);

    node1->page_base = 0x100000;
    node2->page_base = 0x200000;

    skiplist_insert(test_scope, 0x100000, idx1);
    skiplist_insert(test_scope, 0x200000, idx2);

    uint16_t old_head = header->skip_list_head;

    // Remove head
    int result = skiplist_remove(test_scope, old_head);

    Assert.isTrue(result == OK, "Remove head should succeed");
    Assert.isTrue(header->skip_list_head != old_head, "Head should change");
    Assert.isTrue(header->page_count == 1, "Should have 1 page remaining");
}

void test_pagelist_remove_middle(void) {
    reset_nodepool();
    // Insert three pages
    uint16_t indices[3];
    addr addrs[] = {0x100000, 0x200000, 0x300000};

    for (int i = 0; i < 3; i++) {
        indices[i] = alloc_page_node_slot();
        page_node *node = get_page_node(indices[i]);
        node->page_base = addrs[i];
        skiplist_insert(test_scope, addrs[i], indices[i]);
    }

    nodepool_header *header = get_header();
    Assert.isTrue(header->page_count == 3, "Should have 3 pages");

    // Remove middle page (0x200000)
    int result = skiplist_remove(test_scope, indices[1]);

    Assert.isTrue(result == OK, "Remove middle should succeed");
    Assert.isTrue(header->page_count == 2, "Should have 2 pages");

    // Verify the other two are still connected
    uint16_t current = header->skip_list_head;
    int count = 0;
    while (current != PAGE_NODE_NULL && count < 10) {
        page_node *node = get_page_node(current);
        Assert.isTrue(node->page_base != 0x200000, "Removed page should not be in list");
        current = node->forward[0];
        count++;
    }

    Assert.isTrue(count == 2, "Should traverse 2 nodes");
}
#endif

#if 1  // Region: Test Registration
__attribute__((constructor)) void init_pagelist_tests(void) {
    testset("MTIS: PageList (Skip List) Operations", set_config, set_teardown);

    // Insert tests
    testcase("PageList: insert single page", test_pagelist_insert_single);
    testcase("PageList: insert maintains address order", test_pagelist_insert_maintains_order);
    testcase("PageList: insert multiple pages", test_pagelist_insert_multiple);

    // Find tests
    testcase("PageList: find containing page", test_pagelist_find_containing);
    testcase("PageList: find containing boundary", test_pagelist_find_containing_boundary);
    testcase("PageList: find containing not found", test_pagelist_find_containing_not_found);
    testcase("PageList: find for size", test_pagelist_find_for_size);

    // Remove tests
    testcase("PageList: remove single page", test_pagelist_remove_single);
    testcase("PageList: remove head page", test_pagelist_remove_head);
    testcase("PageList: remove middle page", test_pagelist_remove_middle);
}
#endif
