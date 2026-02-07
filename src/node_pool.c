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
 * File: src/node_pool.c
 * Description: Implementation of NodePool for B-Tree nodes
 */

#include "internal/node_pool.h"
#include <string.h>
#include <sys/mman.h>

// Forward declarations - API functions
static int node_pool_init(void);
static void node_pool_shutdown(void);
static node_idx node_pool_alloc_node(void);
static void node_pool_dispose_node(node_idx idx);
static btree_node node_pool_get_node(node_idx idx);
static usize node_pool_capacity(void);
static usize node_pool_free_count(void);
static bool node_pool_is_initialized(void);

// Forward declarations - Helper functions
static void set_pool_base(addr base);
static addr get_pool_base(void);
static void init_free_list(void);

// NodePool state (file-local static variables)
static addr s_pool_base = ADDR_EMPTY;          // Base address of NodePool
static usize s_capacity = 0;                   // Current capacity
static node_idx s_free_list_head = NODE_NULL;  // Free list head

// API function definitions

/**
 * @brief Initialize NodePool
 */
static int node_pool_init(void) {
    // Check if already initialized
    if (get_pool_base() != ADDR_EMPTY) {
        return OK;  // Already initialized
    }

    // Allocate 24KB via mmap (1,024 nodes × 24 bytes)
    void *pool = mmap(NULL, NODE_POOL_INITIAL_SIZE, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (pool == MAP_FAILED) {
        return ERR;
    }

    // Zero the pool
    memset(pool, 0, NODE_POOL_INITIAL_SIZE);

    // Cache base in static (R1 set conditionally)
    set_pool_base((addr)pool);

    // Set capacity
    s_capacity = NODE_POOL_INITIAL_COUNT;

    // Initialize free list (nodes 1-1023)
    init_free_list();

    return OK;
}

/**
 * @brief Shutdown NodePool
 */
static void node_pool_shutdown(void) {
    addr base = get_pool_base();
    if (base == ADDR_EMPTY) {
        return;  // Not initialized
    }

    // Unmap pool
    usize size = s_capacity * NODE_SIZE;
    munmap((void *)base, size);

    // Clear state
    set_pool_base(ADDR_EMPTY);
    s_capacity = 0;
    s_free_list_head = NODE_NULL;
}

/**
 * @brief Allocate a node from free list
 */
static node_idx node_pool_alloc_node(void) {
    node_idx idx = s_free_list_head;
    if (idx == NODE_NULL) {
        return NODE_NULL;  // Pool exhausted
    }

    // Get node
    btree_node node = node_pool_get_node(idx);
    if (node == NULL) {
        return NODE_NULL;
    }

    // Pop from free list (left_idx holds next free node)
    node_idx next_free = node->left_idx;
    s_free_list_head = next_free;

    // Zero the node
    memset(node, 0, NODE_SIZE);

    return idx;
}

/**
 * @brief Free a node back to free list
 */
static void node_pool_dispose_node(node_idx idx) {
    if (idx == NODE_NULL) {
        return;
    }

    btree_node node = node_pool_get_node(idx);
    if (node == NULL) {
        return;
    }

    // Push to front of free list (use left_idx as next pointer)
    node_idx old_head = s_free_list_head;
    node->left_idx = old_head;
    s_free_list_head = idx;
}

/**
 * @brief Get pointer to node by index
 */
static btree_node node_pool_get_node(node_idx idx) {
    if (idx == NODE_NULL) {
        return NULL;
    }

    addr base = get_pool_base();
    if (base == ADDR_EMPTY) {
        return NULL;  // Not initialized
    }

    // Bounds check
    if (idx >= s_capacity) {
        return NULL;  // Out of bounds
    }

    // Fast indexed access: base + (idx * 24)
    return (btree_node)(base + (idx * NODE_SIZE));
}

/**
 * @brief Get total node capacity
 */
static usize node_pool_capacity(void) {
    return s_capacity;
}

/**
 * @brief Get number of free nodes
 */
static usize node_pool_free_count(void) {
    usize count = 0;
    node_idx idx = s_free_list_head;

    while (idx != NODE_NULL) {
        count++;
        btree_node node = node_pool_get_node(idx);
        if (node == NULL) {
            break;
        }
        idx = node->left_idx;  // Next in free list
    }

    return count;
}

/**
 * @brief Check if NodePool is initialized
 */
static bool node_pool_is_initialized(void) {
    return get_pool_base() != ADDR_EMPTY;
}

// API interface definition
const sc_node_pool_i NodePool = {
    .init = node_pool_init,
    .shutdown = node_pool_shutdown,
    .alloc_node = node_pool_alloc_node,
    .dispose_node = node_pool_dispose_node,
    .get_node = node_pool_get_node,
    .capacity = node_pool_capacity,
    .free_count = node_pool_free_count,
    .is_initialized = node_pool_is_initialized,
};

// Helper function definitions

/**
 * @brief Set NodePool base address (static only, R1 optional)
 */
static void set_pool_base(addr base) {
    s_pool_base = base;

    // Optionally cache in R1 register (if SYS0 is ready)
    if (base != ADDR_EMPTY) {
        addr sys0_base = Memory.get_sys0_base();
        if (sys0_base != ADDR_EMPTY) {
            sc_registers *regs = (sc_registers *)sys0_base;
            regs->R1 = base;
        }
    }
}

/**
 * @brief Get NodePool base address
 */
static addr get_pool_base(void) {
    return s_pool_base;
}

/**
 * @brief Initialize free list by linking nodes 1-1023
 */
static void init_free_list(void) {
    // Link nodes 1 through capacity-1
    usize capacity_limit = (s_capacity < 65536) ? s_capacity : 65535;
    for (usize i = 1; i < capacity_limit; i++) {
        btree_node node = node_pool_get_node((node_idx)i);
        if (node == NULL) {
            continue;
        }

        // Link to next node (or NODE_NULL for last)
        node->left_idx = (i + 1 < capacity_limit) ? (node_idx)(i + 1) : NODE_NULL;
    }

    // Set free list head to node 1
    s_free_list_head = 1;
}
