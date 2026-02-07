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

// NodePool constants
#define NODE_POOL_INITIAL_SIZE (24 * 1024)     // 24KB (1,024 nodes × 24 bytes with padding)
#define NODE_POOL_INITIAL_COUNT 1024           // Initial node capacity
#define NODE_SIZE sizeof(sc_node)              // 24 bytes (includes padding)

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
