/*
 * SigmaCore
 * Copyright (c) 2026 David Boarman (BadKraft) and contributors
 * QuantumOverride [Q|]
 * ----------------------------------------------
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 * ----------------------------------------------
 * File: internal/node_pool.h
 * Description: B-Tree node pool management (external metadata)
 *
 * NodePool Architecture:
 * - 18KB separate mmap (1,024 nodes × 18 bytes)
 * - Node 0 reserved as NODE_NULL sentinel
 * - Nodes 1-1023 available for allocation
 * - R1 register caches pool base address
 * - Free list uses left_idx as next pointer
 * - Growable via remapping (18KB → 36KB → 72KB)
 */

#pragma once

#include "internal/memory.h"

// NodePool constants - Dynamic growth philosophy
// Start small (2KB), double on exhaustion: 2KB → 4KB → 8KB → 16KB → 32KB
#define NODE_POOL_INITIAL_SIZE (2 * 1024)  // 2KB initial (embraces dynamic growth)
#define NODE_POOL_INITIAL_COUNT 1024       // Initial node capacity (legacy - unused)
#define NODE_SIZE sizeof(sc_node)          // 24 bytes (cache-friendly padding)

// Phase 7: Initial allocation counts for 2KB NodePool (small start, grows as needed)
// With 40-byte header and 2008 bytes available:
#define INITIAL_PAGE_NODES \
    ((2048 - sizeof(nodepool_header)) / sizeof(page_node))  // ~100 page_nodes (20B each)
#define INITIAL_BTREE_NODES \
    ((2048 - sizeof(nodepool_header)) / sizeof(sc_node))  // ~83 btree_nodes (24B each)

/**
 * @brief NodePool interface
 *
 * Manages external metadata for B-Tree allocation tracking.
 * All nodes indexed via node_idx (uint16_t).
 */
typedef struct sc_node_pool_i {
    /**
     * @brief Initialize NodePool
     * @return OK on success, ERR on failure
     *
     * Allocates 24KB via mmap, initializes free list,
     * caches base address in R1 register.
     */
    int (*init)(void);

    /**
     * @brief Shutdown NodePool
     *
     * Unmaps NodePool memory, clears R1 register.
     */
    void (*shutdown)(void);

    /**
     * @brief Allocate a node from free list
     * @return node_idx of allocated node, NODE_NULL if pool exhausted
     *
     * Pops first free node from list, zeroes the node.
     */
    node_idx (*alloc_node)(void);

    /**
     * @brief Free a node back to free list
     * @param idx Node index to free
     *
     * Pushes node to front of free list (free list uses left_idx as next pointer).
     * Node is NOT zeroed (caller may reuse fields).
     */
    void (*dispose_node)(node_idx idx);

    /**
     * @brief Get pointer to node by index
     * @param idx Node index (1-1023)
     * @return Pointer to sc_node, or NULL if idx == NODE_NULL
     *
     * Fast indexed access: base + (idx * 18)
     */
    btree_node (*get_node)(node_idx idx);

    /**
     * @brief Get total node capacity
     * @return Current capacity (initially 1024)
     */
    usize (*capacity)(void);

    /**
     * @brief Get number of free nodes
     * @return Count of nodes on free list
     */
    usize (*free_count)(void);

    /**
     * @brief Check if NodePool is initialized
     * @return true if initialized, false otherwise
     */
    bool (*is_initialized)(void);
} sc_node_pool_i;

extern const sc_node_pool_i NodePool;

// Phase 7: Per-scope NodePool functions (two-tier architecture)

/**
 * @brief Initialize per-scope NodePool
 * @param scope_ptr Pointer to sc_scope structure
 * @return OK on success, ERR on failure
 *
 * Allocates 8KB mmap region, initializes nodepool_header,
 * sets up empty skip list, initializes allocation offsets.
 * Updates scope->nodepool_base with mmap address.
 */
int nodepool_init(scope scope_ptr);

/**
 * @brief Shutdown per-scope NodePool
 * @param scope_ptr Pointer to sc_scope structure
 *
 * Unmaps NodePool memory, clears scope->nodepool_base.
 */
void nodepool_shutdown(scope scope_ptr);

/**
 * @brief Allocate a page_node from per-scope pool
 * @param scope_ptr Pointer to sc_scope structure
 * @return Index of allocated page_node, or PAGE_NODE_NULL if exhausted
 *
 * Allocates from bottom-up region (after header).
 * Grows pool via mremap if collision with btree nodes detected.
 */
uint16_t nodepool_alloc_page_node(scope scope_ptr);

/**
 * @brief Allocate a btree_node from per-scope pool
 * @param scope_ptr Pointer to sc_scope structure
 * @return Index of allocated btree_node, or NODE_NULL if exhausted
 *
 * Allocates from top-down region (before end of mmap).
 * Grows pool via mremap if collision with page nodes detected.
 */
node_idx nodepool_alloc_btree_node(scope scope_ptr);

/**
 * @brief Get pointer to page_node by index
 * @param scope_ptr Pointer to sc_scope structure
 * @param idx Page node index
 * @return Pointer to page_node, or NULL if invalid
 */
page_node *nodepool_get_page_node(scope scope_ptr, uint16_t idx);

/**
 * @brief Get pointer to btree_node by index (per-scope)
 * @param scope_ptr Pointer to sc_scope structure
 * @param idx Node index
 * @return Pointer to sc_node, or NULL if invalid
 */
btree_node nodepool_get_btree_node(scope scope_ptr, node_idx idx);

/**
 * @brief Get NodePool header for scope
 * @param scope_ptr Pointer to sc_scope structure
 * @return Pointer to nodepool_header, or NULL if not initialized
 */
nodepool_header *nodepool_get_header(scope scope_ptr);

// ============================================================================
// Phase 7: Skip List Operations (Page Directory)
// ============================================================================

/**
 * @brief Insert page into skip list (address-ordered)
 * @param scope_ptr Scope containing NodePool
 * @param page_base Base address of page to insert
 * @param page_idx Index of page_node to insert
 * @return OK on success, ERR on failure
 */
int skiplist_insert(scope scope_ptr, addr page_base, uint16_t page_idx);

/**
 * @brief Find page containing given address
 * @param scope_ptr Scope containing NodePool
 * @param address Address to search for
 * @param page_idx_out Output: index of containing page_node (if found)
 * @return OK if found, ERR if not found
 */
int skiplist_find_containing(scope scope_ptr, addr address, uint16_t *page_idx_out);

/**
 * @brief Find page with space for allocation of given size
 * @param scope_ptr Scope containing NodePool
 * @param size Required allocation size
 * @param page_idx_out Output: index of suitable page_node (if found)
 * @return OK if found, ERR if no suitable page
 */
int skiplist_find_for_size(scope scope_ptr, usize size, uint16_t *page_idx_out);

/**
 * @brief Remove page from skip list
 * @param scope_ptr Scope containing NodePool
 * @param page_idx Index of page_node to remove
 * @return OK on success, ERR on failure
 */
int skiplist_remove(scope scope_ptr, uint16_t page_idx);

// ============================================================================
// Phase 7: Per-Page B-Tree Operations (MTIS Tier 2)
// ============================================================================

/**
 * @brief Insert allocation into page's B-tree
 * @param scope_ptr Scope containing NodePool
 * @param page_idx Index of page_node containing the B-tree
 * @param start Allocation start address
 * @param length Allocation size in bytes
 * @param out_node_idx Output: index of created btree_node (optional, can be NULL)
 * @return OK on success, ERR on failure
 *
 * Allocates a btree_node, inserts into page's B-tree maintaining BST property,
 * increments page->block_count.
 */
int btree_page_insert(scope scope_ptr, uint16_t page_idx, addr start, usize length,
                      node_idx *out_node_idx);

/**
 * @brief Search for allocation by address in page's B-tree
 * @param scope_ptr Scope containing NodePool
 * @param page_idx Index of page_node containing the B-tree
 * @param start Allocation start address to find
 * @param out_node_idx Output: index of found btree_node (optional, can be NULL)
 * @return OK if found, ERR if not found
 */
int btree_page_search(scope scope_ptr, uint16_t page_idx, addr start, node_idx *out_node_idx);

/**
 * @brief Delete allocation from page's B-tree
 * @param scope_ptr Scope containing NodePool
 * @param page_idx Index of page_node containing the B-tree
 * @param delete_idx Index of btree_node to delete
 * @return OK on success, ERR on failure
 *
 * Removes node from B-tree (standard BST deletion),
 * decrements page->block_count.
 */
int btree_page_delete(scope scope_ptr, uint16_t page_idx, node_idx delete_idx);

/**
 * @brief Coalesce a freed block with adjacent free blocks
 * @param scope_ptr Scope containing NodePool
 * @param page_idx Index of page_node containing the B-tree
 * @param freed_idx Index of freshly freed block
 * @return OK if coalescing performed, ERR if no coalescing possible
 *
 * Checks in-order predecessor and successor for adjacency.
 * If adjacent and free, merges into a single larger block.
 */
int btree_page_coalesce(scope scope_ptr, uint16_t page_idx, node_idx freed_idx);

/**
 * @brief Find best-fit free block in page's B-tree
 * @param scope_ptr Scope containing NodePool
 * @param page_idx Index of page_node containing the B-tree
 * @param size Minimum required size
 * @param out_node_idx Output: index of suitable free btree_node (optional, can be NULL)
 * @return OK if found, ERR if no suitable block
 *
 * Searches for smallest free block >= size using max_free hints.
 */
int btree_page_find_free(scope scope_ptr, uint16_t page_idx, usize size, node_idx *out_node_idx);
