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

#define _GNU_SOURCE  // Required for mremap()
#include "internal/node_pool.h"
#include <stdio.h>
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
        // If R1 is not set but sys0_base is now available, set R1 now
        addr sys0_base = Memory.get_sys0_base();
        if (sys0_base != ADDR_EMPTY) {
            sc_registers *regs = (sc_registers *)sys0_base;
            if (regs->R1 == ADDR_EMPTY) {
                regs->R1 = get_pool_base();
            }
        }

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

// ============================================================================
// Phase 7: Per-Scope NodePool Implementation (Two-Tier Architecture)
// ============================================================================

/**
 * @brief Initialize per-scope NodePool
 *
 * Allocates 8KB mmap region with layout:
 * [Header 38B][page_nodes grow up][btree_nodes grow down]
 *
 * Header tracks allocation offsets. When they collide, mremap doubles capacity.
 */
int nodepool_init(scope scope_ptr) {
    if (scope_ptr == NULL) {
        return ERR;
    }

    // Check if already initialized
    if (scope_ptr->nodepool_base != ADDR_EMPTY) {
        return OK;  // Already initialized
    }

    // Allocate 8KB via mmap
    void *pool = mmap(NULL, NODE_POOL_INITIAL_SIZE, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (pool == MAP_FAILED) {
        return ERR;
    }

    // Zero the pool
    memset(pool, 0, NODE_POOL_INITIAL_SIZE);

    // Initialize header
    nodepool_header *header = (nodepool_header *)pool;
    header->capacity = NODE_POOL_INITIAL_SIZE;
    header->page_count = 0;
    header->page_alloc_offset = sizeof(nodepool_header);  // Start after header
    header->btree_alloc_offset = NODE_POOL_INITIAL_SIZE;  // Start from end
    header->skip_list_head = PAGE_NODE_NULL;

    // Save base address in scope
    scope_ptr->nodepool_base = (addr)pool;

    return OK;
}

/**
 * @brief Shutdown per-scope NodePool
 */
void nodepool_shutdown(scope scope_ptr) {
    if (scope_ptr == NULL || scope_ptr->nodepool_base == ADDR_EMPTY) {
        return;  // Not initialized
    }

    // Get header to read capacity
    nodepool_header *header = (nodepool_header *)scope_ptr->nodepool_base;
    usize size = header->capacity;

    // Unmap pool
    munmap((void *)scope_ptr->nodepool_base, size);

    // Clear scope reference
    scope_ptr->nodepool_base = ADDR_EMPTY;
}

/**
 * @brief Allocate a page_node from bottom-up region
 */
uint16_t nodepool_alloc_page_node(scope scope_ptr) {
    if (scope_ptr == NULL || scope_ptr->nodepool_base == ADDR_EMPTY) {
        return PAGE_NODE_NULL;
    }

    nodepool_header *header = (nodepool_header *)scope_ptr->nodepool_base;

    // Check for collision with btree nodes growing down
    usize next_page_offset = header->page_alloc_offset + sizeof(page_node);
    if (next_page_offset >= header->btree_alloc_offset) {
        // GROW POOL via mremap (v0.2.2)
        usize old_capacity = header->capacity;
        usize new_capacity = old_capacity * 2;  // Double capacity

        void *new_base = mremap((void *)scope_ptr->nodepool_base, old_capacity, new_capacity,
                                MREMAP_MAYMOVE  // Allow kernel to move if necessary
        );

        if (new_base == MAP_FAILED) {
            // Growth failed - out of memory
            return PAGE_NODE_NULL;
        }

        // Update scope reference (may have moved)
        scope_ptr->nodepool_base = (addr)new_base;
        header = (nodepool_header *)new_base;

        // Update header capacity
        header->capacity = new_capacity;

        // btree_alloc_offset needs adjustment (grows from top)
        // Old: capacity - btree_bytes_used
        // New: new_capacity - btree_bytes_used
        usize btree_bytes_used = old_capacity - header->btree_alloc_offset;

        // CRITICAL: Move existing btree node data to new top region
        // Before: [old_base ... existing_btree_nodes  old_capacity]
        // After:  [new_base ... [empty space] ... existing_btree_nodes new_capacity]
        if (btree_bytes_used > 0) {
            void *old_btree_start = (void *)((addr)header + header->btree_alloc_offset);
            void *new_btree_start = (void *)((addr)header + (new_capacity - btree_bytes_used));
            memmove(new_btree_start, old_btree_start, btree_bytes_used);
        }

        // Update btree_alloc_offset to point at new top location
        header->btree_alloc_offset = new_capacity - btree_bytes_used;

        // Recalculate collision check with new capacity
        next_page_offset = header->page_alloc_offset + sizeof(page_node);
    }

    // Calculate index (page_nodes start at index 1, index 0 is PAGE_NODE_NULL sentinel)
    usize base_offset = sizeof(nodepool_header);
    usize idx = (header->page_alloc_offset - base_offset) / sizeof(page_node) + 1;

    // Advance allocation offset
    header->page_alloc_offset = next_page_offset;

    // Zero the page_node
    page_node *pnode = nodepool_get_page_node(scope_ptr, (uint16_t)idx);
    if (pnode != NULL) {
        memset(pnode, 0, sizeof(page_node));
    }

    return (uint16_t)idx;
}

/**
 * @brief Allocate a btree_node from top-down region
 */
node_idx nodepool_alloc_btree_node(scope scope_ptr) {
    if (scope_ptr == NULL || scope_ptr->nodepool_base == ADDR_EMPTY) {
        return NODE_NULL;
    }

    nodepool_header *header = (nodepool_header *)scope_ptr->nodepool_base;

    // Check for collision with page nodes growing up
    usize next_btree_offset = header->btree_alloc_offset - sizeof(sc_node);
    if (next_btree_offset <= header->page_alloc_offset) {
        // GROW POOL via mremap (Phase 1 Day 2: NPB-01/02/03)
        usize old_capacity = header->capacity;
        usize new_capacity = old_capacity * 2;

        void *new_base =
            mremap((void *)scope_ptr->nodepool_base, old_capacity, new_capacity, MREMAP_MAYMOVE);

        if (new_base == MAP_FAILED) {
            return NODE_NULL;
        }

        // Update scope and header
        scope_ptr->nodepool_base = (addr)new_base;
        header = (nodepool_header *)new_base;
        header->capacity = new_capacity;

        // Adjust btree_alloc_offset: btree grows from top, so we need to maintain
        // the same "distance from top" in the new larger capacity
        usize btree_bytes_used = old_capacity - header->btree_alloc_offset;

        // CRITICAL: Move existing btree node data to new top region
        // Before: [old_base ... existing_btree_nodes  old_capacity]
        // After:  [new_base ... [empty space] ... existing_btree_nodes new_capacity]
        if (btree_bytes_used > 0) {
            void *old_btree_start = (void *)((addr)header + header->btree_alloc_offset);
            void *new_btree_start = (void *)((addr)header + (new_capacity - btree_bytes_used));
            memmove(new_btree_start, old_btree_start, btree_bytes_used);
        }

        // Update btree_alloc_offset to point at new top location
        header->btree_alloc_offset = new_capacity - btree_bytes_used;

        // Recalculate with new capacity
        next_btree_offset = header->btree_alloc_offset - sizeof(sc_node);
    }

    // Advance allocation offset (grows down)
    header->btree_alloc_offset = next_btree_offset;

    // Calculate index (btree_nodes index from top)
    // Node 0 reserved as NODE_NULL sentinel
    // Actual nodes start from capacity and grow down
    usize capacity_nodes = header->capacity / sizeof(sc_node);
    usize nodes_from_top = (header->capacity - next_btree_offset) / sizeof(sc_node);
    usize idx = capacity_nodes - nodes_from_top;

    // Zero the btree_node
    btree_node bnode = nodepool_get_btree_node(scope_ptr, (node_idx)idx);
    if (bnode != NULL) {
        memset(bnode, 0, sizeof(sc_node));
    }

    return (node_idx)idx;
}

/**
 * @brief Get pointer to page_node by index
 */
page_node *nodepool_get_page_node(scope scope_ptr, uint16_t idx) {
    if (scope_ptr == NULL || scope_ptr->nodepool_base == ADDR_EMPTY) {
        return NULL;
    }

    if (idx == PAGE_NODE_NULL) {
        return NULL;
    }

    // Calculate offset: header + ((idx - 1) * sizeof(page_node))
    // Index 1 is at offset=header, index 2 at offset=header+sizeof(page_node), etc.
    addr base = scope_ptr->nodepool_base;
    usize offset = sizeof(nodepool_header) + ((idx - 1) * sizeof(page_node));

    // Bounds check against page_alloc_offset
    nodepool_header *header = (nodepool_header *)base;
    if (offset >= header->page_alloc_offset) {
        return NULL;  // Beyond allocated region
    }

    return (page_node *)(base + offset);
}

/**
 * @brief Get pointer to btree_node by index
 */
btree_node nodepool_get_btree_node(scope scope_ptr, node_idx idx) {
    if (scope_ptr == NULL || scope_ptr->nodepool_base == ADDR_EMPTY) {
        return NULL;
    }

    if (idx == NODE_NULL) {
        return NULL;
    }

    nodepool_header *header = (nodepool_header *)scope_ptr->nodepool_base;

    // Calculate offset from top: capacity - (idx * sizeof(sc_node))
    addr base = scope_ptr->nodepool_base;
    usize capacity_nodes = header->capacity / sizeof(sc_node);
    usize offset = header->capacity - ((capacity_nodes - idx) * sizeof(sc_node));

    // Bounds check against btree_alloc_offset
    if (offset < header->btree_alloc_offset) {
        return NULL;  // Beyond allocated region
    }

    return (btree_node)(base + offset);
}

/**
 * @brief Get NodePool header for scope
 */
nodepool_header *nodepool_get_header(scope scope_ptr) {
    if (scope_ptr == NULL || scope_ptr->nodepool_base == ADDR_EMPTY) {
        return NULL;
    }

    return (nodepool_header *)scope_ptr->nodepool_base;
}

// ============================================================================
// Phase 7: Skip List Operations (Page Directory)
// ============================================================================

/**
 * @brief Generate random level for skip list node (probabilistic)
 * @return Level 0-3 (50% L0, 25% L1, 12.5% L2, 6.25% L3)
 */
static int skiplist_random_level(void) {
    int level = 0;
    // Simple random using low bits of address (fast, good enough for skip list)
    static unsigned int seed = 0x12345678;
    seed = seed * 1103515245 + 12345;
    unsigned int r = seed;

    while ((r & 1) && level < SKIP_LIST_MAX_LEVEL - 1) {
        level++;
        r >>= 1;
    }
    return level;
}

/**
 * @brief Insert page into skip list (address-ordered)
 * @param scope_ptr Scope containing NodePool
 * @param page_base Base address of page to insert
 * @param page_idx Index of page_node to insert
 * @return OK on success, ERR on failure
 */
int skiplist_insert(scope scope_ptr, addr page_base, uint16_t page_idx) {
    if (scope_ptr == NULL || scope_ptr->nodepool_base == ADDR_EMPTY) {
        return ERR;
    }

    nodepool_header *header = nodepool_get_header(scope_ptr);
    if (header == NULL) {
        return ERR;
    }

    page_node *new_node = nodepool_get_page_node(scope_ptr, page_idx);
    if (new_node == NULL) {
        return ERR;
    }

    // Initialize new node
    new_node->page_base = page_base;
    new_node->btree_root = NODE_NULL;
    new_node->block_count = 0;

    // Generate random level for this node
    int level = skiplist_random_level();

    // Empty list case
    if (header->skip_list_head == PAGE_NODE_NULL) {
        // New node is the only node
        for (int i = 0; i <= level; i++) {
            new_node->forward[i] = PAGE_NODE_NULL;
        }
        header->skip_list_head = page_idx;
        header->page_count = 1;
        return OK;
    }

    // Find insert position (update[i] = node before insert point at level i)
    uint16_t update[SKIP_LIST_MAX_LEVEL];

    // Initialize update to PAGE_NODE_NULL (means insert before head)
    for (int i = 0; i < SKIP_LIST_MAX_LEVEL; i++) {
        update[i] = PAGE_NODE_NULL;
    }

    // Start from highest level, descend to lowest
    uint16_t current_idx = header->skip_list_head;

    for (int i = SKIP_LIST_MAX_LEVEL - 1; i >= 0; i--) {
        // Start position for this level
        if (i < SKIP_LIST_MAX_LEVEL - 1) {
            // For levels below top, start from previous level's position
            if (update[i + 1] != PAGE_NODE_NULL) {
                current_idx = update[i + 1];
            } else {
                current_idx = header->skip_list_head;
            }
        }

        // Move forward while next node exists and has smaller address
        while (current_idx != PAGE_NODE_NULL) {
            page_node *current = nodepool_get_page_node(scope_ptr, current_idx);
            if (current == NULL) break;

            // If current >= new value, insert before it (leave update[i] = NULL)
            if (current->page_base >= page_base) {
                break;
            }

            uint16_t next_idx = current->forward[i];
            if (next_idx == PAGE_NODE_NULL) {
                // End of list, insert after current
                update[i] = current_idx;
                break;
            }

            page_node *next = nodepool_get_page_node(scope_ptr, next_idx);
            if (next == NULL) {
                update[i] = current_idx;
                break;
            }

            if (next->page_base >= page_base) {
                // Insert between current and next
                update[i] = current_idx;
                break;
            }

            current_idx = next_idx;
        }
    }

    // Insert at position (after update[0])
    // Link new node into each level
    for (int i = 0; i <= level; i++) {
        if (update[i] == PAGE_NODE_NULL) {
            // Insert before current head
            new_node->forward[i] = header->skip_list_head;
        } else {
            page_node *prev = nodepool_get_page_node(scope_ptr, update[i]);
            if (prev != NULL) {
                new_node->forward[i] = prev->forward[i];
                prev->forward[i] = page_idx;
            }
        }
    }

    // Update head if inserting before current head
    if (update[0] == PAGE_NODE_NULL) {
        header->skip_list_head = page_idx;
    }

    // Clear higher levels (not used for this node)
    for (int i = level + 1; i < SKIP_LIST_MAX_LEVEL; i++) {
        new_node->forward[i] = PAGE_NODE_NULL;
    }

    header->page_count++;
    return OK;
}

/**
 * @brief Find page containing given address
 * @param scope_ptr Scope containing NodePool
 * @param address Address to search for
 * @param page_idx_out Output: index of containing page_node
 * @return OK if found, ERR if not found
 */
int skiplist_find_containing(scope scope_ptr, addr address, uint16_t *page_idx_out) {
    if (scope_ptr == NULL || scope_ptr->nodepool_base == ADDR_EMPTY || page_idx_out == NULL) {
        return ERR;
    }

    nodepool_header *header = nodepool_get_header(scope_ptr);
    if (header == NULL || header->skip_list_head == PAGE_NODE_NULL) {
        return ERR;
    }

    // Descend skip list from top level
    uint16_t current_idx = header->skip_list_head;
    page_node *current = NULL;

    for (int i = SKIP_LIST_MAX_LEVEL - 1; i >= 0; i--) {
        while (current_idx != PAGE_NODE_NULL) {
            current = nodepool_get_page_node(scope_ptr, current_idx);
            if (current == NULL) return ERR;

            // Check if address is in current page
            if (address >= current->page_base && address < current->page_base + SYS0_PAGE_SIZE) {
                *page_idx_out = current_idx;
                return OK;
            }

            // Check next node at this level
            uint16_t next_idx = current->forward[i];
            if (next_idx == PAGE_NODE_NULL) break;

            page_node *next = nodepool_get_page_node(scope_ptr, next_idx);
            if (next == NULL) break;

            // If next page starts beyond target address, descend
            if (next->page_base > address) {
                break;
            }

            current_idx = next_idx;
        }
    }

    return ERR;  // Not found
}

/**
 * @brief Find page with space for allocation of given size
 * @param scope_ptr Scope containing NodePool
 * @param size Required allocation size
 * @param page_idx_out Output: index of suitable page_node
 * @return OK if found, ERR if no suitable page
 *
 * Strategy: First-fit - scan skip list level 0 for first page with space
 * Future optimization: Track max_free per page for O(log n) search
 */
int skiplist_find_for_size(scope scope_ptr, usize size, uint16_t *page_idx_out) {
    if (scope_ptr == NULL || scope_ptr->nodepool_base == ADDR_EMPTY || page_idx_out == NULL) {
        return ERR;
    }

    nodepool_header *header = nodepool_get_header(scope_ptr);
    if (header == NULL || header->skip_list_head == PAGE_NODE_NULL) {
        return ERR;
    }

    // TODO (Task 7): Use size parameter for smarter page selection
    (void)size;  // Suppress warning - will use after B-tree integration

    // Scan level 0 (full list) for page with space
    uint16_t current_idx = header->skip_list_head;

    while (current_idx != PAGE_NODE_NULL) {
        page_node *current = nodepool_get_page_node(scope_ptr, current_idx);
        if (current == NULL) return ERR;

        // Check if page has room
        // TODO (Task 7): Query per-page B-tree for max_free
        // For now, simple heuristic: if block_count < max blocks per page
        usize max_blocks_per_page = SYS0_PAGE_SIZE / 32;  // Conservative estimate
        if (current->block_count < max_blocks_per_page) {
            *page_idx_out = current_idx;
            return OK;
        }

        current_idx = current->forward[0];  // Next at level 0
    }

    return ERR;  // No suitable page found
}

/**
 * @brief Remove page from skip list
 * @param scope_ptr Scope containing NodePool
 * @param page_idx Index of page_node to remove
 * @return OK on success, ERR on failure
 *
 * NOTE: For page release (v0.4.0+), munmap page after removal
 */
int skiplist_remove(scope scope_ptr, uint16_t page_idx) {
    if (scope_ptr == NULL || scope_ptr->nodepool_base == ADDR_EMPTY) {
        return ERR;
    }

    nodepool_header *header = nodepool_get_header(scope_ptr);
    if (header == NULL || header->skip_list_head == PAGE_NODE_NULL) {
        return ERR;
    }

    page_node *target = nodepool_get_page_node(scope_ptr, page_idx);
    if (target == NULL) {
        return ERR;
    }

    // Find update pointers (nodes that point to target)
    uint16_t update[SKIP_LIST_MAX_LEVEL];
    uint16_t current_idx = header->skip_list_head;

    for (int i = 0; i < SKIP_LIST_MAX_LEVEL; i++) {
        update[i] = PAGE_NODE_NULL;
    }

    // If removing head, special case
    if (current_idx == page_idx) {
        // Head becomes the next node at level 0 (or NULL if no next)
        header->skip_list_head = target->forward[0];

        if (header->page_count > 0) {
            header->page_count--;
        }
        return OK;
    }

    // Find predecessors at each level
    for (int i = SKIP_LIST_MAX_LEVEL - 1; i >= 0; i--) {
        current_idx = header->skip_list_head;

        while (current_idx != PAGE_NODE_NULL && current_idx != page_idx) {
            page_node *current = nodepool_get_page_node(scope_ptr, current_idx);
            if (current == NULL) break;

            if (current->forward[i] == page_idx) {
                update[i] = current_idx;
                break;
            }

            current_idx = current->forward[i];
        }
    }

    // Unlink target from each level
    for (int i = 0; i < SKIP_LIST_MAX_LEVEL; i++) {
        if (update[i] != PAGE_NODE_NULL) {
            page_node *prev = nodepool_get_page_node(scope_ptr, update[i]);
            if (prev != NULL && prev->forward[i] == page_idx) {
                prev->forward[i] = target->forward[i];
            }
        }
    }

    if (header->page_count > 0) {
        header->page_count--;
    }

    return OK;
}

// ============================================================================
// Per-Page B-Tree Operations (MTIS Tier 2)
// ============================================================================

/**
 * @brief Recursive BST insertion helper
 * @param scope_ptr Scope containing NodePool
 * @param current_idx Current node being examined
 * @param new_idx Node to insert
 * @return OK on success, ERR on failure (duplicate address)
 */
static int btree_insert_recursive(scope scope_ptr, node_idx current_idx, node_idx new_idx) {
    btree_node current = nodepool_get_btree_node(scope_ptr, current_idx);
    btree_node new_node = nodepool_get_btree_node(scope_ptr, new_idx);

    if (current == NULL || new_node == NULL) {
        return ERR;
    }

    // BST property: left < current < right (by start address)
    if (new_node->start < current->start) {
        // Insert in left subtree
        if (current->left_idx == NODE_NULL) {
            current->left_idx = new_idx;
            return OK;
        }
        return btree_insert_recursive(scope_ptr, current->left_idx, new_idx);
    } else if (new_node->start > current->start) {
        // Insert in right subtree
        if (current->right_idx == NODE_NULL) {
            current->right_idx = new_idx;
            return OK;
        }
        return btree_insert_recursive(scope_ptr, current->right_idx, new_idx);
    } else {
        // Duplicate address - not allowed
        return ERR;
    }
}

/**
 * @brief Insert allocation into page's B-tree
 */
int btree_page_insert(scope scope_ptr, uint16_t page_idx, addr start, usize length,
                      node_idx *out_node_idx) {
    if (scope_ptr == NULL || scope_ptr->nodepool_base == ADDR_EMPTY) {
        return ERR;
    }

    // Validate length (must fit in uint32_t, nonzero)
    if (length == 0 || length > 0xFFFFFFFF) {
        return ERR;
    }

    page_node *page = nodepool_get_page_node(scope_ptr, page_idx);
    if (page == NULL) {
        return ERR;
    }

    // Allocate new btree_node
    node_idx new_idx = nodepool_alloc_btree_node(scope_ptr);
    if (new_idx == NODE_NULL) {
        return ERR;  // Pool exhausted
    }

    // Initialize node
    btree_node new_node = nodepool_get_btree_node(scope_ptr, new_idx);
    if (new_node == NULL) {
        return ERR;
    }

    new_node->start = start;
    new_node->length = (uint32_t)length;
    new_node->left_idx = NODE_NULL;
    new_node->right_idx = NODE_NULL;
    new_node->info = 0;  // Allocated node (no FREE_FLAG)

    // If tree is empty, new node becomes root
    if (page->btree_root == NODE_NULL) {
        page->btree_root = new_idx;
        page->block_count = 1;

        if (out_node_idx != NULL) {
            *out_node_idx = new_idx;
        }

        return OK;
    }

    // Insert into non-empty tree using BST insertion
    int result = btree_insert_recursive(scope_ptr, page->btree_root, new_idx);
    if (result != OK) {
        // TODO: Free the allocated node back to pool
        return ERR;
    }

    // Update block count
    page->block_count++;

    if (out_node_idx != NULL) {
        *out_node_idx = new_idx;
    }

    return OK;
}

/**
 * @brief Search for allocation by address in page's B-tree
 */
int btree_page_search(scope scope_ptr, uint16_t page_idx, addr start, node_idx *out_node_idx) {
    if (scope_ptr == NULL || scope_ptr->nodepool_base == ADDR_EMPTY) {
        return ERR;
    }

    page_node *page = nodepool_get_page_node(scope_ptr, page_idx);
    if (page == NULL) {
        return ERR;
    }

    // Empty tree
    if (page->btree_root == NODE_NULL) {
        return ERR;
    }

    // Search from root
    node_idx current_idx = page->btree_root;

    while (current_idx != NODE_NULL) {
        btree_node current = nodepool_get_btree_node(scope_ptr, current_idx);
        if (current == NULL) {
            return ERR;
        }

        if (start == current->start) {
            // Found exact match
            if (out_node_idx != NULL) {
                *out_node_idx = current_idx;
            }
            return OK;
        } else if (start < current->start) {
            // Search left subtree
            current_idx = current->left_idx;
        } else {
            // Search right subtree
            current_idx = current->right_idx;
        }
    }

    // Not found
    return ERR;
}

/**
 * @brief Find minimum node in subtree (leftmost node)
 */
static node_idx btree_find_min(scope scope_ptr, node_idx idx) {
    if (idx == NODE_NULL) {
        return NODE_NULL;
    }

    node_idx current = idx;
    btree_node node = nodepool_get_btree_node(scope_ptr, current);

    while (node != NULL && node->left_idx != NODE_NULL) {
        current = node->left_idx;
        node = nodepool_get_btree_node(scope_ptr, current);
    }

    return current;
}

/**
 * @brief Find maximum node in subtree (rightmost node)
 */
static node_idx btree_find_max(scope scope_ptr, node_idx idx) {
    if (idx == NODE_NULL) {
        return NODE_NULL;
    }

    node_idx current = idx;
    btree_node node = nodepool_get_btree_node(scope_ptr, current);

    while (node != NULL && node->right_idx != NODE_NULL) {
        current = node->right_idx;
        node = nodepool_get_btree_node(scope_ptr, current);
    }

    return current;
}

/**
 * @brief Find in-order predecessor of a node
 */
static node_idx btree_find_predecessor(scope scope_ptr, node_idx root, node_idx target_idx) {
    if (root == NODE_NULL || target_idx == NODE_NULL) {
        return NODE_NULL;
    }

    btree_node target = nodepool_get_btree_node(scope_ptr, target_idx);
    if (target == NULL) {
        return NODE_NULL;
    }

    // If left subtree exists, predecessor is max of left subtree
    if (target->left_idx != NODE_NULL) {
        return btree_find_max(scope_ptr, target->left_idx);
    }

    // Otherwise, search from root for predecessor
    node_idx predecessor = NODE_NULL;
    node_idx current = root;

    while (current != NODE_NULL && current != target_idx) {
        btree_node node = nodepool_get_btree_node(scope_ptr, current);
        if (node == NULL) {
            break;
        }

        if (target->start > node->start) {
            predecessor = current;  // Current is smaller, could be predecessor
            current = node->right_idx;
        } else {
            current = node->left_idx;
        }
    }

    return predecessor;
}

/**
 * @brief Find in-order successor of a node
 */
static node_idx btree_find_successor(scope scope_ptr, node_idx root, node_idx target_idx) {
    if (root == NODE_NULL || target_idx == NODE_NULL) {
        return NODE_NULL;
    }

    btree_node target = nodepool_get_btree_node(scope_ptr, target_idx);
    if (target == NULL) {
        return NODE_NULL;
    }

    // If right subtree exists, successor is min of right subtree
    if (target->right_idx != NODE_NULL) {
        return btree_find_min(scope_ptr, target->right_idx);
    }

    // Otherwise, search from root for successor
    node_idx successor = NODE_NULL;
    node_idx current = root;

    while (current != NODE_NULL && current != target_idx) {
        btree_node node = nodepool_get_btree_node(scope_ptr, current);
        if (node == NULL) {
            break;
        }

        if (target->start < node->start) {
            successor = current;  // Current is larger, could be successor
            current = node->left_idx;
        } else {
            current = node->right_idx;
        }
    }

    return successor;
}

/**
 * @brief Recursive BST deletion helper
 * @param scope_ptr Scope containing NodePool
 * @param current_idx Current node being examined
 * @param delete_idx Node index to delete
 * @param deleted_out Output: set to true if node was deleted
 * @return New root of this subtree after deletion
 */
static node_idx btree_delete_recursive(scope scope_ptr, node_idx current_idx, node_idx delete_idx,
                                       bool *deleted_out) {
    if (current_idx == NODE_NULL) {
        *deleted_out = false;
        return NODE_NULL;
    }

    btree_node current = nodepool_get_btree_node(scope_ptr, current_idx);
    if (current == NULL) {
        *deleted_out = false;
        return current_idx;
    }

    // Found the node to delete
    if (current_idx == delete_idx) {
        *deleted_out = true;

        // Case 1: Leaf node (no children)
        if (current->left_idx == NODE_NULL && current->right_idx == NODE_NULL) {
            // Free node back to pool
            NodePool.dispose_node(current_idx);
            return NODE_NULL;
        }

        // Case 2: Single child
        if (current->left_idx == NODE_NULL) {
            // Only right child
            node_idx result = current->right_idx;
            // Free node back to pool
            NodePool.dispose_node(current_idx);
            return result;
        }
        if (current->right_idx == NODE_NULL) {
            // Only left child
            node_idx result = current->left_idx;
            // Free node back to pool
            NodePool.dispose_node(current_idx);
            return result;
        }

        // Case 3: Two children - replace with in-order successor (min of right subtree)
        node_idx successor_idx = btree_find_min(scope_ptr, current->right_idx);
        btree_node successor = nodepool_get_btree_node(scope_ptr, successor_idx);

        if (successor != NULL) {
            // Copy successor's data to current node
            current->start = successor->start;
            current->length = successor->length;
            current->info = successor->info;

            // Delete successor from right subtree
            bool dummy;
            current->right_idx =
                btree_delete_recursive(scope_ptr, current->right_idx, successor_idx, &dummy);
        }

        return current_idx;
    }

    // Recursively search for node to delete
    btree_node delete_node = nodepool_get_btree_node(scope_ptr, delete_idx);
    if (delete_node == NULL) {
        *deleted_out = false;
        return current_idx;
    }

    if (delete_node->start < current->start) {
        // Search left subtree
        current->left_idx =
            btree_delete_recursive(scope_ptr, current->left_idx, delete_idx, deleted_out);
    } else {
        // Search right subtree
        current->right_idx =
            btree_delete_recursive(scope_ptr, current->right_idx, delete_idx, deleted_out);
    }

    return current_idx;
}

/**
 * @brief Delete a node from the page B-tree
 * @param scope_ptr Scope containing NodePool
 * @param page_idx Index of page_node containing the B-tree
 * @param delete_idx Index of node to delete
 * @return OK on success, ERR on failure
 */
int btree_page_delete(scope scope_ptr, uint16_t page_idx, node_idx delete_idx) {
    if (scope_ptr == NULL || page_idx == NODE_NULL || delete_idx == NODE_NULL) {
        return ERR;
    }

    page_node *page = nodepool_get_page_node(scope_ptr, page_idx);
    if (page == NULL) {
        return ERR;
    }

    // Empty tree
    if (page->btree_root == NODE_NULL) {
        return ERR;
    }

    // Delete node from tree
    bool deleted = false;
    page->btree_root = btree_delete_recursive(scope_ptr, page->btree_root, delete_idx, &deleted);

    if (!deleted) {
        return ERR;
    }

    // Update block count
    if (page->block_count > 0) {
        page->block_count--;
    }

    return OK;
}

/**
 * @brief Coalesce a freed block with adjacent free blocks
 * @param scope_ptr Scope containing NodePool
 * @param page_idx Index of page_node containing the B-tree
 * @param freed_idx Index of the freshly freed block
 * @return OK if coalescing performed, ERR if not
 *
 * Checks predecessor and successor for adjacency. If adjacent and free,
 * merges them into a single larger block.
 */
static int btree_coalesce_block(scope scope_ptr, uint16_t page_idx, node_idx freed_idx) {
    if (scope_ptr == NULL || freed_idx == NODE_NULL) {
        return ERR;
    }

    page_node *page = nodepool_get_page_node(scope_ptr, page_idx);
    if (page == NULL || page->btree_root == NODE_NULL) {
        return ERR;
    }

    sc_node *freed = nodepool_get_btree_node(scope_ptr, freed_idx);
    if (freed == NULL || !(freed->info & NODE_FREE_FLAG)) {
        return ERR;  // Must be marked free
    }

    bool coalesced = false;

    // Try to coalesce with predecessor (left neighbor in address space)
    node_idx pred_idx = btree_find_predecessor(scope_ptr, page->btree_root, freed_idx);
    if (pred_idx != NODE_NULL) {
        sc_node *pred = nodepool_get_btree_node(scope_ptr, pred_idx);
        if (pred != NULL && (pred->info & NODE_FREE_FLAG)) {
            // Check if adjacent: pred.start + pred.length == freed.start
            if (pred->start + pred->length == freed->start) {
                // Merge: extend predecessor, delete freed block
                pred->length += freed->length;
                btree_page_delete(scope_ptr, page_idx, freed_idx);
                freed_idx = pred_idx;  // Continue with merged block
                freed = pred;
                coalesced = true;
            }
        }
    }

    // Try to coalesce with successor (right neighbor in address space)
    node_idx succ_idx = btree_find_successor(scope_ptr, page->btree_root, freed_idx);
    if (succ_idx != NODE_NULL) {
        sc_node *succ = nodepool_get_btree_node(scope_ptr, succ_idx);
        if (succ != NULL && (succ->info & NODE_FREE_FLAG)) {
            // Check if adjacent: freed.start + freed.length == succ.start
            if (freed->start + freed->length == succ->start) {
                // Merge: extend freed block, delete successor
                freed->length += succ->length;
                btree_page_delete(scope_ptr, page_idx, succ_idx);
                coalesced = true;
            }
        }
    }

    return coalesced ? OK : ERR;
}

/**
 * @brief Coalesce a freed block with adjacent free blocks
 */
int btree_page_coalesce(scope scope_ptr, uint16_t page_idx, node_idx freed_idx) {
    return btree_coalesce_block(scope_ptr, page_idx, freed_idx);
}

// Helper: Recursively find best-fit free block
static void btree_find_best_fit_recursive(scope scope_ptr, node_idx current_idx, usize size,
                                          node_idx *best_idx, usize *best_size) {
    if (current_idx == NODE_NULL) {
        return;
    }

    sc_node *node = nodepool_get_btree_node(scope_ptr, current_idx);
    if (node == NULL) {
        return;
    }

    // Check if this node is free and fits
    if ((node->info & NODE_FREE_FLAG) && node->length >= size) {
        // This is a candidate - is it better than current best?
        if (*best_idx == NODE_NULL || node->length < *best_size) {
            *best_idx = current_idx;
            *best_size = node->length;
        }
    }

    // Recursively search both subtrees
    btree_find_best_fit_recursive(scope_ptr, node->left_idx, size, best_idx, best_size);
    btree_find_best_fit_recursive(scope_ptr, node->right_idx, size, best_idx, best_size);
}

/**
 * @brief Find best-fit free block in page's B-tree
 */
int btree_page_find_free(scope scope_ptr, uint16_t page_idx, usize size, node_idx *out_node_idx) {
    if (scope_ptr == NULL || scope_ptr->nodepool_base == ADDR_EMPTY || size == 0) {
        return ERR;
    }

    page_node *page = nodepool_get_page_node(scope_ptr, page_idx);
    if (page == NULL || page->btree_root == NODE_NULL) {
        return ERR;
    }

    // Search for best-fit free block
    node_idx best_idx = NODE_NULL;
    usize best_size = 0;
    btree_find_best_fit_recursive(scope_ptr, page->btree_root, size, &best_idx, &best_size);

    if (best_idx == NODE_NULL) {
        return ERR;  // No suitable free block found
    }

    if (out_node_idx != NULL) {
        *out_node_idx = best_idx;
    }

    return OK;
}
