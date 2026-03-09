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
 * File: memory.c
 * Description: SigmaCore memory management implementation
 */

#include "internal/memory.h"
#include "internal/node_pool.h"
#include "internal/slab_manager.h"
#include "memory.h"
// ----------------
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

// Register indices
#define REG_CURRENT_SCOPE 7
#define REG_SYS0_BASE 0

// Forward declarations
static object sys0_alloc(usize size);
static void sys0_dispose(object ptr);
static object slb0_alloc(usize size);
static void slb0_dispose(object ptr);
static object slb0_realloc(object ptr, usize new_size);
block_header memory_get_first_header(void);
static object memory_alloc_for_scope(void *scope_ptr, usize size);
static void memory_dispose_for_scope(void *scope_ptr, object ptr);
scope get_scope_table_entry(usize index);

// Frame operations (v0.2.1)
static frame frame_begin_impl(void);
static integer frame_end_impl(frame f);
static usize frame_depth_impl(void);
static usize frame_allocated_impl(frame f);
static object alloc_from_frame(scope scope_ptr, usize size);
static int register_large_alloc_with_frame(scope scope_ptr, addr alloc_addr, usize alloc_size);

// Arena operations (v0.2.2)
static scope arena_create_impl(const char *name, sbyte policy);
static void arena_dispose_impl(scope s);
static object arena_alloc(scope arena, usize size);
static void arena_dispose_ptr(scope arena, object ptr);
static page_sentinel arena_alloc_new_page(scope arena);

// Scope prev-chain operations (v0.2.3)
static void scope_restore(void);
static bool scope_owns_frame(scope s, frame f);

// Scope-generic page allocator with MTIS registration (v0.2.3)
static page_sentinel scope_alloc_tracked_page(scope s);

// Frame operations (v0.2.3 - explicit scope)
static frame frame_begin_in_impl(scope s);
static integer frame_end_in_impl(scope s, frame f);
static usize frame_depth_of_impl(scope s);

// Arena operations (v0.2.3 - explicit scope)
static scope arena_find_impl(const char *name);

static void *sys0 = NULL;
static sbyte sys_page[SYS0_PAGE_SIZE] __attribute__((aligned(SYS0_PAGE_SIZE)));
// Scope table lives in SYS0 data area after header block
// scope_table[0]=SYS0, scope_table[1]=SLB0, [2-15]=user arenas
static scope scope_table = NULL;

// Internal register access functions
static addr registers_get(sbyte index) {
    registers regs = (registers)(sys_page + SYS0_REGISTERS_OFFSET);
    if (index < 0 || index > 7) {
        return 0;  // Invalid index
    }
    // Access register array via pointer arithmetic
    return ((addr *)regs)[index];
}
static void registers_set(sbyte index, addr value) {
    registers regs = (registers)(sys_page + SYS0_REGISTERS_OFFSET);
    if (index < 0 || index > 7) {
        return;  // Invalid index, no-op
    }
    // Set register array via pointer arithmetic
    ((addr *)regs)[index] = value;
}
// Get SYS0 page base address from R0
static addr get_sys0_base(void) {
    return registers_get(REG_SYS0_BASE);
}

// SYS0 reclaiming allocation (first-fit with splitting)
static object sys0_alloc(usize size) {
    object ptr = NULL;
    block_header hdr = memory_get_first_header();
    if (hdr == NULL || size == 0) {
        goto exit;
    }

    // Align size to kAlign boundary
    usize aligned = (size + (kAlign - 1)) & ~(kAlign - 1);

    // Define minimum remainder to avoid tiny fragments
    const usize MIN_REMAINDER = sizeof(sc_blk_header) + kAlign;

    // First-fit search
    while (hdr != NULL) {
        if ((hdr->flags & BLK_FLAG_FREE) && hdr->size >= aligned) {
            usize free_size = hdr->size;

            // Check if we can split the block
            if (free_size >= aligned + sizeof(sc_blk_header) + MIN_REMAINDER) {
                // === SPLIT BLOCK ===
                // Calculate offsets
                addr payload_off = (addr)hdr + sizeof(sc_blk_header);
                addr new_hdr_off = payload_off + aligned;

                // Update allocated block header
                hdr->size = aligned;
                hdr->flags &=
                    ~(BLK_FLAG_FREE | BLK_FLAG_LAST | BLK_FLAG_FOOT);  // Mark as allocated
                hdr->next_off = (uint32_t)(new_hdr_off - (addr)sys_page);

                // Create new free block header
                block_header new_hdr = (block_header)new_hdr_off;
                new_hdr->next_off = 0;  // Will be last block
                new_hdr->size = (uint32_t)(free_size - aligned - sizeof(sc_blk_header));
                new_hdr->flags = BLK_FLAG_FREE | BLK_FLAG_LAST | BLK_FLAG_FOOT;

                ptr = (object)payload_off;
            } else {
                // === NO-SPLIT PATH ===
                hdr->next_off = 0;  // No next block
                hdr->size = (uint32_t)free_size;
                hdr->flags &=
                    ~(BLK_FLAG_FREE | BLK_FLAG_LAST | BLK_FLAG_FOOT);  // Mark as allocated

                ptr = (object)((addr)hdr + sizeof(sc_blk_header));
            }

            break;
        }
        // Move to next block header
        if (hdr->next_off == 0) {
            break;  // No more blocks
        }
        hdr = (block_header)((addr)sys_page + hdr->next_off);
    }

exit:
    return ptr;
}
// SYS0 disposal — intentional no-op.
// All SYS0 allocations (scope table, bootstrap objects) are designed to live
// for the full lifetime of the memory instance.  They are released implicitly
// when the process exits; individual disposal is not supported.
static void sys0_dispose(object ptr) {
    (void)ptr;
}

// Helper: Bump allocate from a page
static object slb0_bump_alloc(page_sentinel page, usize aligned_size) {
    addr page_base = (addr)page;
    addr page_end = page_base + SYS0_PAGE_SIZE;
    addr alloc_start = page_base + page->bump_offset;
    addr alloc_end = alloc_start + aligned_size;

    if (alloc_end > page_end) {
        return NULL;  // Page exhausted
    }

    page->bump_offset += aligned_size;
    page->alloc_count++;
    return (object)alloc_start;
}

// Helper: Allocate a new page dynamically and register it
static page_sentinel slb0_alloc_new_page(scope slb0) {
    if (slb0 == NULL) {
        return NULL;
    }

    // Allocate a new 8KB page via mmap
    void *new_page =
        mmap(NULL, SYS0_PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (new_page == MAP_FAILED) {
        return NULL;
    }

    // Initialize page sentinel
    page_sentinel ps = (page_sentinel)new_page;
    ps->scope_id = slb0->scope_id;
    ps->page_index = (sbyte)slb0->page_count;  // Sequential index
    ps->flags = 0;
    ps->bump_offset = sizeof(sc_page_sentinel);  // 32 bytes
    ps->_pad[0] = 0;
    ps->_pad[1] = 0;
    ps->_pad[2] = 0;
    ps->_pad[3] = 0;
    ps->_pad[4] = 0;
    ps->_pad[5] = 0;
    ps->free_list_head = 0;
    ps->alloc_count = 0;
    ps->next_page_off = 0;  // Will be last page

    // Find the last page in the chain and link to it
    page_sentinel last_page = (page_sentinel)slb0->first_page_off;
    if (last_page != NULL) {
        while (last_page->next_page_off != 0) {
            last_page = (page_sentinel)last_page->next_page_off;
        }
        last_page->next_page_off = (addr)new_page;
    } else {
        // First page ever (shouldn't happen for SLB0, but handle it)
        slb0->first_page_off = (addr)new_page;
    }

    // Register in skip list (MTIS Tier 1)
    uint16_t page_idx = nodepool_alloc_page_node(slb0);
    if (page_idx == PAGE_NODE_NULL) {
        // Failed to allocate page_node - unmap and return NULL
        munmap(new_page, SYS0_PAGE_SIZE);
        return NULL;
    }

    page_node *pn = nodepool_get_page_node(slb0, page_idx);
    if (pn == NULL) {
        // Failed to get page_node - unmap and return NULL
        munmap(new_page, SYS0_PAGE_SIZE);
        return NULL;
    }

    // Initialize page_node
    pn->page_base = (addr)new_page;
    pn->btree_root = NODE_NULL;
    pn->block_count = 0;
    for (int i = 0; i < SKIP_LIST_MAX_LEVEL; i++) {
        pn->forward[i] = PAGE_NODE_NULL;
    }

    // Insert into skip list
    skiplist_insert(slb0, (addr)new_page, page_idx);

    // Update scope page count
    slb0->page_count++;

    return ps;
}

// SLB0 allocation (B-Tree search + bump pointer fallback)
static object slb0_alloc(usize size) {
    object ptr = NULL;
    bool register_with_frame = false;  // Track if we need to register large allocation
    scope slb0 = get_scope_table_entry(1);
    if (slb0 == NULL || size == 0) {
        goto exit;
    }

    // Enforce minimum allocation size and alignment
    usize aligned_size = size < SLB0_MIN_ALLOC ? SLB0_MIN_ALLOC : size;
    aligned_size = (aligned_size + (kAlign - 1)) & ~(kAlign - 1);

    // Check if in frame context - try frame allocation first
    // Fall back to normal allocation if frame can't handle it (e.g., too large)
    if (slb0->frame_active) {
        ptr = alloc_from_frame(slb0, aligned_size);
        if (ptr != NULL) {
            goto exit;  // Frame allocation succeeded
        }
        // Fall through to normal allocation for oversized requests
        // Mark that we need to register this with the frame
        register_with_frame = true;
    }

    // MTIS Tier 1: Find page with space using skip list
    uint16_t page_idx = PAGE_NODE_NULL;
    if (skiplist_find_for_size(slb0, aligned_size, &page_idx) == OK) {
        page_node *pn = nodepool_get_page_node(slb0, page_idx);
        if (pn == NULL) {
            goto fallback_bump;
        }

        // MTIS Tier 2: Search page's B-tree for free block (best-fit)
        node_idx free_node = NODE_NULL;
        if (btree_page_find_free(slb0, page_idx, aligned_size, &free_node) == OK) {
            sc_node *node = nodepool_get_btree_node(slb0, free_node);
            if (node != NULL && node->length >= aligned_size) {
                ptr = (object)node->start;

                // Split block if remainder is useful
                if (node->length > aligned_size + SLB0_MIN_ALLOC) {
                    addr remainder_start = node->start + aligned_size;
                    usize remainder_length = node->length - aligned_size;

                    // BUGFIX: Reuse existing node for allocated block instead of delete+insert
                    // This prevents NodePool exhaustion from split operations
                    node->length = aligned_size;
                    node->info &= ~NODE_FREE_FLAG;  // Mark as allocated

                    // Insert remainder as new free block
                    node_idx remainder_idx = NODE_NULL;
                    btree_page_insert(slb0, page_idx, remainder_start, remainder_length,
                                      &remainder_idx);
                    if (remainder_idx != NODE_NULL) {
                        sc_node *remainder = nodepool_get_btree_node(slb0, remainder_idx);
                        if (remainder != NULL) {
                            remainder->info |= NODE_FREE_FLAG;  // Mark as free
                        }
                    }
                } else {
                    // Use entire block - just mark as allocated
                    node->info &= ~NODE_FREE_FLAG;  // Clear free flag
                }

                goto exit;
            }
        }

        // No free blocks in B-tree, try bump allocation from this page
        page_sentinel ps = (page_sentinel)pn->page_base;
        ptr = slb0_bump_alloc(ps, aligned_size);
        if (ptr != NULL) {
            // Track allocation in MTIS
            btree_page_insert(slb0, page_idx, (addr)ptr, aligned_size, NULL);
            goto exit;
        }
    }

fallback_bump:
    // Fallback: Legacy bump allocation from page chain
    page_sentinel page = (page_sentinel)slb0->current_page_off;
    if (page == NULL) {
        page = (page_sentinel)slb0->first_page_off;
    }

    while (page != NULL) {
        ptr = slb0_bump_alloc(page, aligned_size);
        if (ptr != NULL) {
            slb0->current_page_off = (addr)page;

            // Track allocation in MTIS
            uint16_t tracking_page_idx = PAGE_NODE_NULL;
            if (skiplist_find_containing(slb0, (addr)ptr, &tracking_page_idx) == OK) {
                btree_page_insert(slb0, tracking_page_idx, (addr)ptr, aligned_size, NULL);
            }

            goto exit;
        }

        if (page->next_page_off == 0) {
            break;
        }
        page = (page_sentinel)page->next_page_off;
    }

    // DYNAMIC policy - allocate new page and chain it
    page_sentinel new_page = slb0_alloc_new_page(slb0);
    if (new_page != NULL) {
        ptr = slb0_bump_alloc(new_page, aligned_size);
        if (ptr != NULL) {
            slb0->current_page_off = (addr)new_page;

            // Track allocation in MTIS
            uint16_t new_page_idx = PAGE_NODE_NULL;
            if (skiplist_find_containing(slb0, (addr)ptr, &new_page_idx) == OK) {
                btree_page_insert(slb0, new_page_idx, (addr)ptr, aligned_size, NULL);
            }
        }
    }

exit:
    // Register large allocations with frame if we're in frame context
    // and allocation succeeded via normal path (not alloc_from_frame)
    if (register_with_frame && ptr != NULL) {
        register_large_alloc_with_frame(slb0, (addr)ptr, aligned_size);
    }
    
    return ptr;
}

// SLB0 disposal (MTIS-based deallocation)
static void slb0_dispose(object ptr) {
    if (ptr == NULL) {
        return;
    }

    scope slb0 = get_scope_table_entry(1);
    if (slb0 == NULL) {
        return;
    }

    // MTIS Tier 1: Find page containing this address
    uint16_t page_idx = PAGE_NODE_NULL;
    if (skiplist_find_containing(slb0, (addr)ptr, &page_idx) != OK) {
        return;  // Not in any tracked page
    }

    page_node *pn = nodepool_get_page_node(slb0, page_idx);
    if (pn == NULL) {
        return;
    }

    // MTIS Tier 2: Find allocation in page's B-tree
    node_idx alloc_node = NODE_NULL;
    if (btree_page_search(slb0, page_idx, (addr)ptr, &alloc_node) != OK) {
        return;  // Not tracked in B-tree
    }

    // Mark block as free
    sc_node *node = nodepool_get_btree_node(slb0, alloc_node);
    if (node != NULL) {
        node->info |= NODE_FREE_FLAG;  // Set free flag
    }

    // Coalesce with adjacent free blocks
    btree_page_coalesce(slb0, page_idx, alloc_node);

    // Decrement allocation count on page sentinel
    page_sentinel page = (page_sentinel)pn->page_base;
    if (page != NULL && page->alloc_count > 0) {
        page->alloc_count--;
    }

    // Release dynamically-allocated empty pages back to the OS.
    // Guard: only pages beyond the initial 16-page reservation (contiguous mmap)
    // can be individually munmap'd.  The initial block is released wholesale on
    // shutdown via shutdown_memory_system().
    if (page != NULL && page->alloc_count == 0) {
        addr initial_end = slb0->first_page_off + (16u * SYS0_PAGE_SIZE);
        if (pn->page_base >= initial_end) {
            // Purge B-tree nodes — returns them to the nodepool free list
            btree_page_purge(slb0, page_idx);

            // Unlink from the page chain.
            // The predecessor's next_page_off must be updated before munmap so
            // that slb0_alloc's fallback_bump walk never touches freed memory.
            addr page_next = page->next_page_off;
            if (slb0->first_page_off == pn->page_base) {
                // Releasing the very first page (unusual, but handle it)
                slb0->first_page_off = page_next;
            } else {
                page_sentinel prev = (page_sentinel)slb0->first_page_off;
                while (prev != NULL && prev->next_page_off != pn->page_base) {
                    prev = (prev->next_page_off != 0)
                               ? (page_sentinel)prev->next_page_off
                               : NULL;
                }
                if (prev != NULL) {
                    prev->next_page_off = page_next;
                }
            }

            // Redirect current_page_off away from this (soon-to-be-unmapped) page
            if (slb0->current_page_off == pn->page_base) {
                slb0->current_page_off = slb0->first_page_off;
            }
            // Remove page from skip list directory
            skiplist_remove(slb0, page_idx);
            // Zero the page_node entry (marks it as inactive)
            addr page_base = pn->page_base;
            memset(pn, 0, sizeof(page_node));
            // Unmap the data page
            munmap((void *)page_base, SYS0_PAGE_SIZE);
            // Update scope page count
            if (slb0->page_count > 0) {
                slb0->page_count--;
            }
            return;  // pn is no longer valid; done
        }
    }
}

// SLB0 realloc: in-place shrink when possible; alloc+copy+dispose for growth.
// Passing NULL ptr is equivalent to alloc(new_size).
// Passing new_size == 0 is equivalent to dispose(ptr), returns NULL.
static object slb0_realloc(object ptr, usize new_size) {
    if (ptr == NULL) {
        return (new_size > 0) ? slb0_alloc(new_size) : NULL;
    }
    if (new_size == 0) {
        slb0_dispose(ptr);
        return NULL;
    }

    scope slb0 = get_scope_table_entry(1);
    if (slb0 == NULL) {
        return NULL;
    }

    // Locate the page and B-tree node tracking this allocation
    uint16_t page_idx = PAGE_NODE_NULL;
    if (skiplist_find_containing(slb0, (addr)ptr, &page_idx) != OK) {
        return NULL;  // not a tracked SLB0 allocation
    }
    node_idx alloc_node = NODE_NULL;
    if (btree_page_search(slb0, page_idx, (addr)ptr, &alloc_node) != OK) {
        return NULL;
    }
    sc_node *node = nodepool_get_btree_node(slb0, alloc_node);
    if (node == NULL) {
        return NULL;
    }

    usize old_size    = node->length;
    usize aligned_new = new_size < SLB0_MIN_ALLOC ? SLB0_MIN_ALLOC : new_size;
    aligned_new       = (aligned_new + (kAlign - 1)) & ~(kAlign - 1);

    if (aligned_new <= old_size) {
        // Shrink: split remainder into a free block if it is large enough to
        // be useful; otherwise just keep the original block untouched.
        usize remainder = old_size - aligned_new;
        if (remainder >= SLB0_MIN_ALLOC * 2) {
            node->length       = (uint32_t)aligned_new;
            addr rem_start     = (addr)ptr + aligned_new;
            node_idx rem_idx   = NODE_NULL;
            if (btree_page_insert(slb0, page_idx, rem_start, remainder, &rem_idx) == OK
                && rem_idx != NODE_NULL) {
                sc_node *rem = nodepool_get_btree_node(slb0, rem_idx);
                if (rem != NULL) {
                    rem->info |= NODE_FREE_FLAG;
                    btree_page_coalesce(slb0, page_idx, rem_idx);
                }
            }
        }
        return ptr;  // same pointer; block shrunk in-place (or unchanged)
    }

    // Grow: no in-place extension in current architecture — alloc + copy + dispose
    object new_ptr = slb0_alloc(new_size);
    if (new_ptr == NULL) {
        return NULL;  // allocation failed; original pointer still valid
    }
    memcpy(new_ptr, ptr, old_size);
    slb0_dispose(ptr);
    return new_ptr;
}

// Get scope table entry by index
scope get_scope_table_entry(usize index) {
    if (scope_table == NULL || index >= SCOPE_TABLE_COUNT) {
        return NULL;
    }
    return &scope_table[index];
}

#if 1  // Region: Scope Interface Functions
// Get current scope pointer from R7
void *memory_get_current_scope(void) {
    return (void *)registers_get(REG_CURRENT_SCOPE);  // R7 holds the current scope pointer
}

// Restore previous scope via current->prev chain (v0.2.3)
// Clears current->prev so the scope can be re-activated later.
static void scope_restore(void) {
    scope current = (scope)registers_get(REG_CURRENT_SCOPE);
    if (current == NULL) {
        return;
    }
    scope p = current->prev;
    if (p == NULL) {
        return;  // At root (SLB0/SYS0) — no-op
    }
    current->prev = NULL;  // Clear so scope can be set again later
    registers_set(REG_CURRENT_SCOPE, (addr)p);
}

// Set current scope, recording predecessor in s->prev (v0.2.3).
// Returns ERR if s is already active in the chain (s->prev != NULL).
integer memory_set_current_scope(void *scope_ptr) {
    if (scope_ptr == NULL) {
        return ERR;
    }
    scope s = (scope)scope_ptr;
    if (s->prev != NULL) {
        return ERR;  // s already active in chain (prev is set by create or a prior set)
    }
    scope current = (scope)registers_get(REG_CURRENT_SCOPE);
    if (s == current) {
        return OK;  // Already current with no prev — harmless no-op
    }
    s->prev = current;
    registers_set(REG_CURRENT_SCOPE, (addr)s);
    return OK;
}
// Allocate from current scope (dispatch based on scope_id)
object memory_alloc(usize size) {
    object ptr = NULL;
    sc_scope *current_scope = (sc_scope *)memory_get_current_scope();
    if (current_scope == NULL) {
        goto exit;
    }

    // Dispatch based on scope_id
    switch (current_scope->scope_id) {
        case 0:  // SYS0
            ptr = sys0_alloc(size);
            break;
        case 1:  // SLB0
            ptr = slb0_alloc(size);
            break;
        default:  // User arenas (2-15)
            ptr = arena_alloc(current_scope, size);
            break;
    }

exit:
    return ptr;
}
// Dispose to current scope (dispatch based on scope_id)
void memory_dispose(object ptr) {
    sc_scope *current_scope = (sc_scope *)memory_get_current_scope();
    if (current_scope == NULL || ptr == NULL) {
        return;
    }

    // Dispatch based on scope_id
    switch (current_scope->scope_id) {
        case 0:  // SYS0
            sys0_dispose(ptr);
            break;
        case 1:  // SLB0
            slb0_dispose(ptr);
            break;
        default:  // User arenas (2-15)
            arena_dispose_ptr(current_scope, ptr);
            break;
    }
}
// Allocate using an explicit scope
static object memory_alloc_for_scope(void *scope_ptr, usize size) {
    object ptr = NULL;
    sc_scope *explicit_scope = (sc_scope *)scope_ptr;
    if (explicit_scope == NULL) {
        goto exit;
    }

    // Dispatch based on scope_id
    switch (explicit_scope->scope_id) {
        case 0:  // SYS0
            ptr = sys0_alloc(size);
            break;
        case 1:  // SLB0
            ptr = slb0_alloc(size);
            break;
        default:  // User arenas (2-15)
            ptr = arena_alloc(explicit_scope, size);
            break;
    }

exit:
    return ptr;
}
// Dispose using an explicit scope
static void memory_dispose_for_scope(void *scope_ptr, object ptr) {
    sc_scope *explicit_scope = (sc_scope *)scope_ptr;
    if (explicit_scope == NULL || ptr == NULL) {
        return;
    }

    // Dispatch based on scope_id
    switch (explicit_scope->scope_id) {
        case 0:  // SYS0
            sys0_dispose(ptr);
            break;
        case 1:  // SLB0
            slb0_dispose(ptr);
            break;
        default:  // User arenas (2-15)
            arena_dispose_ptr(explicit_scope, ptr);
            break;
    }
}
// Get the current scope configuration
sbyte memory_get_current_scope_config(int mask_type) {
    sbyte result = 0;
    sc_scope *current_scope = (sc_scope *)memory_get_current_scope();
    if (current_scope == NULL) {
        goto exit;
    }

    switch (mask_type) {
        case SCOPE_POLICY:
            result = current_scope->policy;
            break;
        case SCOPE_FLAG:
            result = current_scope->flags;
            break;
        default:
            result = 0;  // Invalid mask_type
            break;
    }
exit:
    return result;
}
// Get configuration from explicit scope
static sbyte memory_get_scope_config(void *scope_ptr, int mask_type) {
    sbyte result = 0;
    sc_scope *explicit_scope = (sc_scope *)scope_ptr;
    if (explicit_scope == NULL) {
        goto exit;
    }

    switch (mask_type) {
        case SCOPE_POLICY:
            result = explicit_scope->policy;
            break;
        case SCOPE_FLAG:
            result = explicit_scope->flags;
            break;
        default:
            result = 0;  // Invalid mask_type
            break;
    }
exit:
    return result;
}
#endif

#if 1  // Region: Internal Memory Functions
// Get SYS0 size
usize memory_sys0_size(void) {
    return sizeof(sys_page);
}
// Check memory state (returns bitfield of MEM_STATE_* flags)
sbyte memory_state(void) {
    sbyte state = 0;
    // Check if memory system is ready
    if (sys0 != NULL && sys0 == (void *)sys_page) {
        state |= MEM_STATE_READY;
    }

    // Check sys0 page alignment
    if (((addr)sys_page % kAlign) == 0) {
        state |= MEM_STATE_ALIGN_SYS0;
    }

    // Check header size alignment
    if ((sizeof(sc_blk_header) % kAlign) == 0) {
        state |= MEM_STATE_ALIGN_HEADER;
    }

    // Check footer placement alignment
    sc_blk_footer *ftr = (sc_blk_footer *)(sys_page + SYS0_PAGE_SIZE - sizeof(sc_blk_footer));
    if (((addr)ftr % _Alignof(uint32_t)) == 0) {
        state |= MEM_STATE_ALIGN_FOOTER;
    }

    // Check if user memory (SLB0) is ready
    scope cur_scope = (scope)memory_get_current_scope();
    if (cur_scope != NULL && cur_scope->scope_id == 1) {
        state |= MEM_STATE_USER_READY;
    }

    // Check if bootstrap is complete
    extern bool __bootstrap_complete;
    if (__bootstrap_complete) {
        state |= MEM_STATE_BOOTSTRAP_COMPLETE;
    }

    return state;
}

// Return pointer to SYS0 first block header
block_header memory_get_first_header(void) {
    block_header hdr = NULL;
    if (!(memory_state() & MEM_STATE_READY)) {
        goto exit;
    }
    hdr = (block_header)(sys_page + FIRST_BLOCK_OFFSET);

exit:
    return hdr;
}
// Return pointer to SYS0 last block footer
block_footer memory_get_last_footer(void) {
    block_footer ftr = NULL;
    if (!(memory_state() & MEM_STATE_READY)) {
        goto exit;
    }
    ftr = (block_footer)(sys_page + LAST_FOOTER_OFFSET);

exit:
    return ftr;
}
// Get slab slot base address
addr memory_get_slots_base(void) {
    return (addr)(sys_page + SYS0_SLAB_TABLE_OFFSET);
}
// Get slab slot end address
addr memory_get_slots_end(void) {
    return (addr)(sys_page + SYS0_SLAB_TABLE_OFFSET + SYS0_SLAB_TABLE_SIZE);
}
#endif

#if 1  // Region: Frame / Scope Helpers (v0.2.3)
/**
 * Allocate a new page for any scope and register it with MTIS (skip list + B-tree).
 * Replaces the SLB0-specific slb0_alloc_new_page for generic scope use.
 *
 * @param s Scope to allocate page in (must have NodePool initialized)
 * @return page_sentinel pointer or NULL on failure
 */
static page_sentinel scope_alloc_tracked_page(scope s) {
    if (s == NULL) {
        return NULL;
    }

    // Allocate a new 8KB page via mmap
    void *new_page =
        mmap(NULL, SYS0_PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (new_page == MAP_FAILED) {
        return NULL;
    }

    // Initialize page sentinel
    page_sentinel ps = (page_sentinel)new_page;
    ps->scope_id = s->scope_id;
    ps->page_index = (sbyte)s->page_count;
    ps->flags = 0;
    ps->bump_offset = sizeof(sc_page_sentinel);
    ps->_pad[0] = 0;
    ps->_pad[1] = 0;
    ps->_pad[2] = 0;
    ps->_pad[3] = 0;
    ps->_pad[4] = 0;
    ps->_pad[5] = 0;
    ps->free_list_head = 0;
    ps->alloc_count = 0;
    ps->next_page_off = 0;

    // Chain to scope's page list
    page_sentinel last = (page_sentinel)s->first_page_off;
    if (last != NULL) {
        while (last->next_page_off != 0) {
            last = (page_sentinel)last->next_page_off;
        }
        last->next_page_off = (addr)new_page;
    } else {
        s->first_page_off = (addr)new_page;
    }

    // Register in skip list (MTIS Tier 1)
    uint16_t page_idx = nodepool_alloc_page_node(s);
    if (page_idx == PAGE_NODE_NULL) {
        munmap(new_page, SYS0_PAGE_SIZE);
        return NULL;
    }

    page_node *pn = nodepool_get_page_node(s, page_idx);
    if (pn == NULL) {
        munmap(new_page, SYS0_PAGE_SIZE);
        return NULL;
    }

    pn->page_base = (addr)new_page;
    pn->btree_root = NODE_NULL;
    pn->block_count = 0;
    for (int i = 0; i < SKIP_LIST_MAX_LEVEL; i++) {
        pn->forward[i] = PAGE_NODE_NULL;
    }

    skiplist_insert(s, (addr)new_page, page_idx);
    s->page_count++;
    s->current_page_off = (addr)new_page;

    return ps;
}

/**
 * Check whether a frame handle belongs to the given scope.
 *
 * @param s Scope to check
 * @param f Frame handle to validate
 * @return true if frame is in scope's frame stack, false otherwise
 */
static bool scope_owns_frame(scope s, frame f) {
    if (s == NULL || !s->frame_active) {
        return false;
    }
    // Packed handle: upper 16 bits = scope_id, lower 16 bits = frame_counter
    uint16_t sid = (uint16_t)((uintptr_t)f >> 16);
    uint16_t fid = (uint16_t)((uintptr_t)f & 0xFFFF);
    return (sid == (uint16_t)s->scope_id) && (fid == s->active_frame.frame_id);
}
#endif

#if 1  // Region: Frame Operations (v0.2.1)
/**
 * Begin a new frame for bulk deallocation.
 * Allocates initial 4KB chunk from free space, uses bump allocation within chunk.
 * Chains additional chunks if frame grows beyond 4KB.
 *
 * @return Opaque frame handle, NULL if max depth exceeded or out of memory
 */
static frame frame_begin_impl(void) {
    scope s = (scope)memory_get_current_scope();  // R7: current scope
    if (s == NULL) {
        return NULL;
    }

    // Enforce single active frame per scope
    if (s->frame_active) {
        return NULL;
    }

    // Find 4KB free block using skip list + B-tree
    uint16_t chunk_idx = NODE_NULL;
    uint16_t page_idx = PAGE_NODE_NULL;

    // Try to find free block via skip list search
    if (skiplist_find_for_size(s, FRAME_CHUNK_SIZE, &page_idx) == OK) {
        if (btree_page_find_free(s, page_idx, FRAME_CHUNK_SIZE, &chunk_idx) != OK) {
            chunk_idx = NODE_NULL;
        }
    }

    // If no free block found, allocate new page
    if (chunk_idx == NODE_NULL) {
        page_sentinel new_page = scope_alloc_tracked_page(s);
        if (new_page == NULL) {
            return NULL;  // Out of memory
        }

        // Find the new page in skip list
        if (skiplist_find_containing(s, (addr)new_page, &page_idx) != OK) {
            return NULL;
        }

        // Bump allocate frame chunk from new page
        addr chunk_addr = (addr)new_page + new_page->bump_offset;
        if (new_page->bump_offset + FRAME_CHUNK_SIZE > SYS0_PAGE_SIZE) {
            return NULL;
        }
        new_page->bump_offset += FRAME_CHUNK_SIZE;

        // Insert node into B-tree to track this frame chunk
        if (btree_page_insert(s, page_idx, chunk_addr, FRAME_CHUNK_SIZE, &chunk_idx) != OK) {
            return NULL;
        }
    }

    // Initialize frame chunk node
    sc_node *frame_node = nodepool_get_btree_node(s, chunk_idx);
    if (frame_node == NULL) {
        return NULL;
    }

    frame_node->info &= ~NODE_FREE_FLAG;
    frame_node->info |= FRAME_NODE_FLAG;
    frame_node->length = FRAME_CHUNK_SIZE;
    frame_node->frame_data.frame_offset = 0;
    frame_node->frame_data.next_chunk_idx = NODE_NULL;
    ++s->frame_counter;
    frame_node->frame_data.frame_id = s->frame_counter;

    // Update scope state
    s->current_frame_idx = chunk_idx;
    s->current_chunk_idx = chunk_idx;

    // Record active frame
    s->active_frame.frame_id          = s->frame_counter;
    s->active_frame.head_chunk_idx    = chunk_idx;
    s->active_frame.large_allocs_head = NODE_NULL;
    s->active_frame.total_allocated   = 0;
    s->frame_active = true;

    // Packed handle: upper 16 bits = scope_id, lower 16 bits = frame_counter
    return (frame)(uintptr_t)(((uintptr_t)s->scope_id << 16) | (uintptr_t)s->frame_counter);
}

/**
 * Allocate from current frame using bump allocation within chunks.
 * Chains new 4KB chunk if current chunk is full.
 *
 * @param scope_ptr Scope pointer (SLB0)
 * @param size Aligned size to allocate
 * @return Pointer to allocated memory, NULL if out of memory
 */
static object alloc_from_frame(scope scope_ptr, usize size) {
    if (scope_ptr == NULL || scope_ptr->current_chunk_idx == NODE_NULL) {
        return NULL;
    }
    
    // Reject allocations larger than chunk size (would corrupt memory)
    // Fall back to normal allocation for large requests
    if (size > FRAME_CHUNK_SIZE) {
        return NULL;  // Caller should use normal Allocator.alloc()
    }
    
    // Get current chunk
    sc_node *chunk = nodepool_get_btree_node(scope_ptr, scope_ptr->current_chunk_idx);
    if (chunk == NULL) {
        return NULL;
    }
    
    // Fast path: fits in current chunk
    if (chunk->frame_data.frame_offset + size <= FRAME_CHUNK_SIZE) {
        addr ptr = chunk->start + chunk->frame_data.frame_offset;
        chunk->frame_data.frame_offset += size;
        scope_ptr->active_frame.total_allocated += size;
        return (object)ptr;
    }
    
    // Slow path: need new chunk
    uint16_t new_chunk_idx = NODE_NULL;
    uint16_t page_idx = PAGE_NODE_NULL;
    
    // Find free block for new chunk
    if (skiplist_find_for_size(scope_ptr, FRAME_CHUNK_SIZE, &page_idx) == OK) {
        if (btree_page_find_free(scope_ptr, page_idx, FRAME_CHUNK_SIZE, &new_chunk_idx) != OK) {
            new_chunk_idx = NODE_NULL;
        }
    }
    
    // Allocate new page if no free block
    if (new_chunk_idx == NODE_NULL) {
        page_sentinel new_page = scope_alloc_tracked_page(scope_ptr);
        if (new_page == NULL) {
            return NULL;  // Out of memory
        }
        
        if (skiplist_find_containing(scope_ptr, (addr)new_page, &page_idx) != OK) {
            return NULL;
        }
        
        // Bump allocate chunk from new page
        addr chunk_addr = (addr)new_page + new_page->bump_offset;
        if (new_page->bump_offset + FRAME_CHUNK_SIZE > SYS0_PAGE_SIZE) {
            return NULL;  // Page doesn't have enough space
        }
        new_page->bump_offset += FRAME_CHUNK_SIZE;
        
        // Insert node to track this chunk
        if (btree_page_insert(scope_ptr, page_idx, chunk_addr, FRAME_CHUNK_SIZE, &new_chunk_idx) != OK) {
            return NULL;
        }
    }
    
    // Initialize new chunk
    sc_node *new_chunk = nodepool_get_btree_node(scope_ptr, new_chunk_idx);
    if (new_chunk == NULL) {
        return NULL;
    }
    
    new_chunk->info &= ~NODE_FREE_FLAG;
    new_chunk->info |= FRAME_NODE_FLAG;
    new_chunk->length = FRAME_CHUNK_SIZE;
    new_chunk->frame_data.frame_offset = size;  // First allocation
    new_chunk->frame_data.next_chunk_idx = NODE_NULL;
    new_chunk->frame_data.frame_id = chunk->frame_data.frame_id;  // Same frame
    
    // Chain to previous chunk
    chunk->frame_data.next_chunk_idx = new_chunk_idx;
    scope_ptr->current_chunk_idx = new_chunk_idx;
    
    // Allocate from new chunk
    scope_ptr->active_frame.total_allocated += size;
    return (object)new_chunk->start;
}

/**
 * Register a large allocation (>4KB) with the current frame for cleanup.
 * Creates a tracking entry that will be freed when frame_end() is called.
 *
 * @param scope_ptr Current scope (SLB0)
 * @param alloc_addr Address of the large allocation
 * @param alloc_size Size of the large allocation
 * @return OK on success, ERR if not in frame context or out of nodes
 */
static int register_large_alloc_with_frame(scope scope_ptr, addr alloc_addr, usize alloc_size) {
    if (scope_ptr == NULL || !scope_ptr->frame_active) {
        return ERR;  // Not in frame context
    }
    
    // Find the B-tree node that tracks this allocation
    uint16_t page_idx = PAGE_NODE_NULL;
    if (skiplist_find_containing(scope_ptr, alloc_addr, &page_idx) != OK) {
        return ERR;  // Can't find page
    }
    
    // Search for the node in the page's B-tree
    node_idx alloc_node_idx = NODE_NULL;
    if (btree_page_search(scope_ptr, page_idx, alloc_addr, &alloc_node_idx) != OK) {
        return ERR;  // Can't find allocation node
    }
    
    sc_node *alloc_node = nodepool_get_btree_node(scope_ptr, alloc_node_idx);
    if (alloc_node == NULL) {
        return ERR;
    }
    
    // Mark as frame-tracked large allocation
    alloc_node->info |= FRAME_LARGE_FLAG;
    
    // Link into frame's large allocation list (LIFO)
    alloc_node->large_alloc_data.next_large_alloc =
        scope_ptr->active_frame.large_allocs_head;
    scope_ptr->active_frame.large_allocs_head = alloc_node_idx;

    // Update total allocated
    scope_ptr->active_frame.total_allocated += alloc_size;
    
    return OK;
}

/**
 * End current frame, marking all chunks as FREE and coalescing.
 * Restores parent frame context if nested.
 *
 * @param f Frame handle from frame_begin()
 * @return OK on success, ERR if invalid frame or not in frame context
 */
static integer frame_end_impl(frame f) {
    scope s = (scope)memory_get_current_scope();  // R7: current scope
    if (s == NULL || !s->frame_active) {
        return ERR;
    }
    if (!scope_owns_frame(s, f)) {
        return ERR;  // Stale or wrong-scope handle
    }

    // Step 1: Free large allocations (>4KB, B-tree allocated)
    uint16_t large_idx = s->active_frame.large_allocs_head;
    while (large_idx != NODE_NULL) {
        sc_node *large_node = nodepool_get_btree_node(s, large_idx);
        if (large_node == NULL) {
            break;
        }
        
        uint16_t next_large = large_node->large_alloc_data.next_large_alloc;
        
        // Mark as free and coalesce
        large_node->info |= NODE_FREE_FLAG;
        large_node->info &= ~FRAME_LARGE_FLAG;
        
        // Find page for coalescing
        uint16_t page_idx = PAGE_NODE_NULL;
        if (skiplist_find_containing(s, large_node->start, &page_idx) == OK) {
            btree_page_coalesce(s, page_idx, large_idx);
        }
        
        large_idx = next_large;
    }
    
    // Step 2: Free frame chunks (4KB, bump allocated)
    uint16_t chunk_idx = s->active_frame.head_chunk_idx;

    // Walk chunk chain and mark all as FREE
    while (chunk_idx != NODE_NULL) {
        sc_node *chunk = nodepool_get_btree_node(s, chunk_idx);
        if (chunk == NULL) {
            break;
        }
        
        uint16_t next = chunk->frame_data.next_chunk_idx;
        
        // Mark chunk as FREE
        chunk->info |= NODE_FREE_FLAG;
        chunk->info &= ~FRAME_NODE_FLAG;
        chunk->frame_data.frame_offset = 0;
        chunk->frame_data.next_chunk_idx = NODE_NULL;
        chunk->frame_data.frame_id = 0;
        
        // Find page containing this chunk for coalescing
        uint16_t page_idx = PAGE_NODE_NULL;
        if (skiplist_find_containing(s, chunk->start, &page_idx) == OK) {
            btree_page_coalesce(s, page_idx, chunk_idx);
        }
        
        chunk_idx = next;
    }
    
    // Clear frame state (no nesting: depth returns to 0)
    s->frame_active = false;
    s->current_frame_idx = NODE_NULL;
    s->current_chunk_idx = NODE_NULL;

    return OK;
}

/**
 * Get current frame nesting depth.
 *
 * @return Frame depth (0 = no active frames, 1+ = nested frames)
 */
static usize frame_depth_impl(void) {
    scope s = (scope)memory_get_current_scope();  // R7: current scope
    if (s == NULL) {
        return 0;
    }
    return s->frame_active ? 1 : 0;
}

/**
 * Get total bytes allocated in a frame.
 *
 * @param f Frame handle
 * @return Total allocated bytes, 0 if invalid frame
 */
static usize frame_allocated_impl(frame f) {
    if (f == NULL) {
        return 0;
    }
    // Self-describing handle: decode scope_id from upper 16 bits
    uint16_t sid = (uint16_t)((uintptr_t)f >> 16);
    uint16_t fid = (uint16_t)((uintptr_t)f & 0xFFFF);
    scope s = get_scope_table_entry(sid);
    if (s == NULL || !s->frame_active || s->active_frame.frame_id != fid) {
        return 0;  // Stale or invalid handle
    }
    return s->active_frame.total_allocated;
}

/**
 * Begin a frame on an explicitly named scope (does NOT change R7).
 *
 * @param scope_ptr Target scope for the frame
 * @return Opaque frame handle, NULL on failure
 */
static frame frame_begin_in_impl(scope s) {
    if (s == NULL) {
        return NULL;
    }

    // Enforce single active frame per scope
    if (s->frame_active) {
        return NULL;
    }

    // Find 4KB free block using skip list + B-tree
    uint16_t chunk_idx = NODE_NULL;
    uint16_t page_idx = PAGE_NODE_NULL;

    if (skiplist_find_for_size(s, FRAME_CHUNK_SIZE, &page_idx) == OK) {
        if (btree_page_find_free(s, page_idx, FRAME_CHUNK_SIZE, &chunk_idx) != OK) {
            chunk_idx = NODE_NULL;
        }
    }

    // If no free block found, allocate new tracked page
    if (chunk_idx == NODE_NULL) {
        page_sentinel new_page = scope_alloc_tracked_page(s);
        if (new_page == NULL) {
            return NULL;
        }

        if (skiplist_find_containing(s, (addr)new_page, &page_idx) != OK) {
            return NULL;
        }

        addr chunk_addr = (addr)new_page + new_page->bump_offset;
        if (new_page->bump_offset + FRAME_CHUNK_SIZE > SYS0_PAGE_SIZE) {
            return NULL;
        }
        new_page->bump_offset += FRAME_CHUNK_SIZE;

        if (btree_page_insert(s, page_idx, chunk_addr, FRAME_CHUNK_SIZE, &chunk_idx) != OK) {
            return NULL;
        }
    }

    // Initialize frame chunk node
    sc_node *frame_node = nodepool_get_btree_node(s, chunk_idx);
    if (frame_node == NULL) {
        return NULL;
    }

    frame_node->info &= ~NODE_FREE_FLAG;
    frame_node->info |= FRAME_NODE_FLAG;
    frame_node->length = FRAME_CHUNK_SIZE;
    frame_node->frame_data.frame_offset = 0;
    frame_node->frame_data.next_chunk_idx = NODE_NULL;
    ++s->frame_counter;
    frame_node->frame_data.frame_id = s->frame_counter;

    // Update scope state
    s->current_frame_idx = chunk_idx;
    s->current_chunk_idx = chunk_idx;

    // Record active frame
    s->active_frame.frame_id          = s->frame_counter;
    s->active_frame.head_chunk_idx    = chunk_idx;
    s->active_frame.large_allocs_head = NODE_NULL;
    s->active_frame.total_allocated   = 0;
    s->frame_active = true;

    // R7 is NOT changed by this function
    // Packed handle: upper 16 bits = scope_id, lower 16 bits = frame_counter
    return (frame)(uintptr_t)(((uintptr_t)s->scope_id << 16) | (uintptr_t)s->frame_counter);
}

/**
 * End a frame on an explicitly named scope (does NOT change R7).
 * Returns ERR if the frame handle does not belong to the given scope.
 *
 * @param scope_ptr Target scope
 * @param f Frame handle from frame_begin_in()
 * @return OK on success, ERR if invalid scope/frame
 */
static integer frame_end_in_impl(scope s, frame f) {
    if (s == NULL || !s->frame_active) {
        return ERR;
    }

    // Validate that this frame belongs to the specified scope
    if (!scope_owns_frame(s, f)) {
        return ERR;
    }

    // Step 1: Free large allocations
    uint16_t large_idx = s->active_frame.large_allocs_head;
    while (large_idx != NODE_NULL) {
        sc_node *large_node = nodepool_get_btree_node(s, large_idx);
        if (large_node == NULL) {
            break;
        }

        uint16_t next_large = large_node->large_alloc_data.next_large_alloc;
        large_node->info |= NODE_FREE_FLAG;
        large_node->info &= ~FRAME_LARGE_FLAG;

        uint16_t page_idx = PAGE_NODE_NULL;
        if (skiplist_find_containing(s, large_node->start, &page_idx) == OK) {
            btree_page_coalesce(s, page_idx, large_idx);
        }

        large_idx = next_large;
    }

    // Step 2: Free frame chunks
    uint16_t chunk_idx = s->active_frame.head_chunk_idx;
    while (chunk_idx != NODE_NULL) {
        sc_node *chunk = nodepool_get_btree_node(s, chunk_idx);
        if (chunk == NULL) {
            break;
        }

        uint16_t next = chunk->frame_data.next_chunk_idx;
        chunk->info |= NODE_FREE_FLAG;
        chunk->info &= ~FRAME_NODE_FLAG;
        chunk->frame_data.frame_offset = 0;
        chunk->frame_data.next_chunk_idx = NODE_NULL;
        chunk->frame_data.frame_id = 0;

        uint16_t page_idx = PAGE_NODE_NULL;
        if (skiplist_find_containing(s, chunk->start, &page_idx) == OK) {
            btree_page_coalesce(s, page_idx, chunk_idx);
        }

        chunk_idx = next;
    }

    // Clear frame state (no nesting: depth returns to 0)
    s->frame_active = false;
    s->current_frame_idx = NODE_NULL;
    s->current_chunk_idx = NODE_NULL;

    // R7 is NOT changed by this function
    return OK;
}

/**
 * Get frame depth of an explicitly named scope (reads scope directly, no R7).
 *
 * @param scope_ptr Target scope
 * @return Frame depth (0 = no active frames)
 */
static usize frame_depth_of_impl(scope s) {
    if (s == NULL) {
        return 0;
    }
    return s->frame_active ? 1 : 0;
}
#endif

#if 1  // Region: Arena Allocation (v  0.2.2 Day 6)
/**
 * Allocate a new page for arena and chain it
 *
 * @param arena Arena scope
 * @return page_sentinel pointer or NULL on failure
 */
static page_sentinel arena_alloc_new_page(scope arena) {
    if (arena == NULL) {
        return NULL;
    }
    
    // Allocate 8KB page via mmap
    page_sentinel new_page = (page_sentinel)mmap(NULL, SYS0_PAGE_SIZE,
                                                   PROT_READ | PROT_WRITE,
                                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (new_page == MAP_FAILED) {
        return NULL;
    }
    
    // Initialize page sentinel
    new_page->bump_offset = sizeof(sc_page_sentinel);  // Start after sentinel
    new_page->alloc_count = 0;
    new_page->next_page_off = 0;  // Will be last page
    
    // Chain to arena's page list
    if (arena->first_page_off == 0) {
        // First page
        arena->first_page_off = (addr)new_page;
        arena->current_page_off = (addr)new_page;
    } else {
        // Append to end of chain
        page_sentinel current = (page_sentinel)arena->current_page_off;
        if (current != NULL) {
            current->next_page_off = (addr)new_page;
        }
        arena->current_page_off = (addr)new_page;
    }
    
    arena->page_count++;
    
    return new_page;
}

/**
 * Allocate from arena using simple bump allocation
 *
 * @param arena Arena scope
 * @param size Requested size in bytes
 * @return Pointer to allocated memory or NULL
 */
static object arena_alloc(scope arena, usize size) {
    if (arena == NULL || size == 0) {
        return NULL;
    }

    // Align size to 16-byte boundary
    usize aligned_size = (size + (kAlign - 1)) & ~(kAlign - 1);

    // If in frame context and fits in chunk, use chunk-based frame allocation
    if (arena->frame_active && aligned_size <= FRAME_CHUNK_SIZE) {
        object ptr = alloc_from_frame(arena, aligned_size);
        if (ptr != NULL) {
            return ptr;
        }
    }

    // Track byte count in frame even for bump-allocated overflow
    if (arena->frame_active) {
        arena->active_frame.total_allocated += aligned_size;
    }

    // Simple bump allocation
    page_sentinel current_page = (page_sentinel)arena->current_page_off;

    // Check if current page has space
    if (current_page != NULL) {
        usize available = SYS0_PAGE_SIZE - current_page->bump_offset;
        if (available >= aligned_size) {
            // Allocate from current page
            object ptr = (object)((addr)current_page + current_page->bump_offset);
            current_page->bump_offset += aligned_size;
            current_page->alloc_count++;
            return ptr;
        }
    }

    // Current page full or no pages exist - allocate new page
    page_sentinel new_page = arena_alloc_new_page(arena);
    if (new_page == NULL) {
        return NULL;
    }

    // Allocate from new page
    object ptr = (object)((addr)new_page + new_page->bump_offset);
    new_page->bump_offset += aligned_size;
    new_page->alloc_count++;

    return ptr;
}

/**
 * Dispose pointer in arena (no-op for now - simple arenas don't reclaim)
 *
 * @param arena Arena scope
 * @param ptr Pointer to dispose
 */
static void arena_dispose_ptr(scope arena, object ptr) {
    // No-op: Simple arenas don't support individual deallocation
    // Memory is freed when entire arena is disposed
    (void)arena;
    (void)ptr;
}
#endif

#if 1  // Region: Arena Lifecycle (v0.2.2)
/**
 * Create a new arena with given name and policy.
 * Finds free slot in scope_table[2-15], creates NodePool, returns scope.
 *
 * @param name Arena name (truncated to 15 chars)
 * @param policy Allocation policy (POOLING, RECLAIMING, GROWABLE)
 * @return scope pointer or NULL on exhaustion
 */
static scope arena_create_impl(const char *name, sbyte policy) {
    scope result = NULL;
    
    // Find free slot in scope_table[2-15]
    for (usize i = 2; i < 16; i++) {
        scope s = get_scope_table_entry(i);
        if (s == NULL || s->nodepool_base == ADDR_EMPTY) {
            // Found free slot
            if (s == NULL) {
                // Slot not allocated, allocate from SYS0
                s = (scope)sys0_alloc(sizeof(sc_scope));
                if (s == NULL) {
                    goto exit;
                }
                // Write to scope_table
                scope_table[i] = *s;
                s = &scope_table[i];
            }
            
            // Initialize scope structure
            memset(s, 0, sizeof(sc_scope));
            s->scope_id = (uint8_t)i;
            s->policy = policy;
            s->flags = 0;  // User arenas are not protected/pinned
            
            // Copy name (truncate to 15 chars + null terminator)
            if (name != NULL) {
                strncpy(s->name, name, 15);
                s->name[15] = '\0';
            } else {
                s->name[0] = '\0';
            }
            
            // Initialize NodePool for this arena
            if (nodepool_init(s) != OK) {
                // NodePool creation failed
                memset(s, 0, sizeof(sc_scope));
                s->nodepool_base = ADDR_EMPTY;
                goto exit;
            }
            
            result = s;
            goto exit;
        }
    }
    
    // No free slots found (exhaustion)
exit:
    // Auto-activate: new arena becomes current scope via prev chain (v0.2.3)
    if (result != NULL) {
        result->prev = (scope)registers_get(REG_CURRENT_SCOPE);
        registers_set(REG_CURRENT_SCOPE, (addr)result);
    }
    return result;
}

/**
 * Dispose arena and free its resources.
 * Auto-unwinds any active frames, restores R7 if arena is current,
 * shuts down NodePool, unmaps all pages, and clears scope_table slot.
 *
 * @param s Scope pointer to dispose
 */
static void arena_dispose_impl(scope s) {
    if (s == NULL) {
        return;
    }
    
    // Validate scope is user arena (2-15)
    uint8_t id = s->scope_id;
    if (id < 2 || id > 15) {
        return;  // Cannot dispose SYS0/SLB0 or invalid scope
    }

    // Auto-unwind active frame if present (v0.2.3: single frame per scope)
    if (s->frame_active) {
        frame f = (frame)(uintptr_t)
            (((uintptr_t)s->scope_id << 16) | (uintptr_t)s->active_frame.frame_id);
        frame_end_in_impl(s, f);
    }

    // Restore R7 to prev only if this arena is currently active (v0.2.3)
    if ((scope)registers_get(REG_CURRENT_SCOPE) == s) {
        scope restore_to = s->prev ? s->prev : get_scope_table_entry(1);  // default SLB0
        registers_set(REG_CURRENT_SCOPE, (addr)restore_to);
    }
    
    // Free all pages in page chain
    addr page_addr = s->first_page_off;
    while (page_addr != 0) {
        page_sentinel page = (page_sentinel)page_addr;
        addr next_addr = page->next_page_off;
        munmap(page, SYS0_PAGE_SIZE);
        page_addr = next_addr;
    }
    
    // Shutdown NodePool
    if (s->nodepool_base != ADDR_EMPTY) {
        nodepool_shutdown(s);
    }
    
    // Clear scope structure
    memset(s, 0, sizeof(sc_scope));
    s->nodepool_base = ADDR_EMPTY;
}

/**
 * Find an existing arena by name.
 *
 * @param name Arena name to search for
 * @return scope pointer or NULL if not found
 */
static scope arena_find_impl(const char *name) {
    if (name == NULL || scope_table == NULL) {
        return NULL;
    }
    for (usize i = 2; i < 16; i++) {
        scope s = get_scope_table_entry(i);
        if (s != NULL && s->nodepool_base != ADDR_EMPTY) {
            if (strncmp(s->name, name, 15) == 0) {
                return s;
            }
        }
    }
    return NULL;
}
#endif

#if 1  // Region: Allocator Interface
// Static sub-interface instances (v0.2.3)
static const sc_frame_i frame_iface = {
    .begin     = frame_begin_impl,
    .end       = frame_end_impl,
    .begin_in  = frame_begin_in_impl,
    .end_in    = frame_end_in_impl,
    .depth     = frame_depth_impl,
    .depth_of  = frame_depth_of_impl,
    .allocated = frame_allocated_impl,
};

static const sc_arena_i arena_iface = {
    .create      = arena_create_impl,
    .dispose     = arena_dispose_impl,
    .find        = arena_find_impl,
    .alloc       = memory_alloc,
    .dispose_ptr = arena_dispose_ptr,
    .frame_begin = frame_begin_in_impl,
    .frame_end   = frame_end_in_impl,
};

const sc_allocator_i Allocator = {
    .alloc   = memory_alloc,
    .dispose = memory_dispose,
    .realloc = slb0_realloc,
    .Scope =
        {
            .current = memory_get_current_scope,
            .set = memory_set_current_scope,
            .restore = scope_restore,
            .config = memory_get_scope_config,
            .alloc = memory_alloc_for_scope,
            .dispose = memory_dispose_for_scope,
        },
    // Backward-compat frame operations (now R7-aware; v0.2.3)
    .frame_begin = frame_begin_impl,
    .frame_end = frame_end_impl,
    .frame_depth = frame_depth_impl,
    .frame_allocated = frame_allocated_impl,
    // Backward-compat arena operations (now R7-aware; v0.2.3)
    .create_arena = arena_create_impl,
    .dispose_arena = arena_dispose_impl,
    // Sub-interfaces (v0.2.3)
    .Frame = frame_iface,
    .Arena = arena_iface,
};
#endif

#if 1  // Region: Internal Memory Interface
const sc_memory_i Memory = {
    .sys0_size = memory_sys0_size,
    .state = memory_state,
    .get_first_header = memory_get_first_header,
    .get_last_footer = memory_get_last_footer,
    .get_sys0_base = get_sys0_base,
    .SlabManager = &SlabManager,
    .get_slots_base = memory_get_slots_base,
    .get_slots_end = memory_get_slots_end,
    .get_scope = get_scope_table_entry,
};
#endif

#if 1  // Region: Memory System Initialization & Shutdown
// Helper to initialize scope_table[0] for SYS0
static void init_scope_table_sys0(void) {
    scope sys0_entry = &scope_table[0];
    // Zero the scope entry to ensure clean initialization
    memset(sys0_entry, 0, sizeof(sc_scope));
    sys0_entry->scope_id = 0;
    sys0_entry->policy = SCOPE_POLICY_RECLAIMING;
    sys0_entry->flags = SCOPE_FLAG_PROTECTED | SCOPE_FLAG_PINNED;
    // Set flags individually to ensure they stick
    sys0_entry->flags |= SCOPE_FLAG_PROTECTED;
    sys0_entry->flags |= SCOPE_FLAG_PINNED;
    sys0_entry->first_page_off = 0;    // SYS0 page base
    sys0_entry->current_page_off = 0;  // Only one page
    sys0_entry->page_count = 1;
    // Copy name inline
    sys0_entry->name[0] = 'S';
    sys0_entry->name[1] = 'Y';
    sys0_entry->name[2] = 'S';
    sys0_entry->name[3] = '0';
    sys0_entry->name[4] = '\0';
    sys0_entry->nodepool_base = ADDR_EMPTY;
}

__attribute__((constructor)) void init_memory_system(void) {
    // Verify SYS0_PAGE_SIZE is a power of 2
    if ((SYS0_PAGE_SIZE & (SYS0_PAGE_SIZE - 1)) != 0) {
        fprintf(stderr, "FATAL: SYS0_PAGE_SIZE (%d) must be a power of 2\n", SYS0_PAGE_SIZE);
        abort();
    }

    // Initialize memory system
    sys0 = (void *)sys_page;

    // Initialize registers (R0-R7)
    registers regs = (registers)(sys_page + SYS0_REGISTERS_OFFSET);
    (void)regs;  // suppress unused variable warning
    for (int i = 0; i < 8; i++) {
        registers_set(i, 0);  // All registers start at 0
    }
    // Set R0 to SYS0 base address for offset calculations
    registers_set(REG_SYS0_BASE, (addr)sys_page);

    // Initialize the header at start of data area (offset 256)
    block_header hdr = (block_header)(sys_page + FIRST_BLOCK_OFFSET);
    hdr->next_off = 0;  // No next block
    hdr->size = SYS0_PAGE_SIZE - FIRST_BLOCK_OFFSET - sizeof(sc_blk_header) - sizeof(sc_blk_footer);
    hdr->flags = BLK_FLAG_FREE | BLK_FLAG_LAST | BLK_FLAG_FOOT;

    // Initialize the footer at end of page
    block_footer ftr = (block_footer)(sys_page + SYS0_PAGE_SIZE - sizeof(sc_blk_footer));
    ftr->magic = BLK_END;
    ftr->size = hdr->size;

    // Allocate scope_table[16] from SYS0 data area FIRST (1024 bytes = 16 * 64)
    // This must happen before SlabManager.init because slotarray allocation uses Allocator
    scope_table = (scope)sys0_alloc(SCOPE_TABLE_COUNT * sizeof(sc_scope));
    if (scope_table == NULL) {
        fprintf(stderr, "FATAL: Unable to allocate scope table\n");
        abort();
    }

    // Initialize scope_table[0] for SYS0
    init_scope_table_sys0();

    // Set R7 to point to scope_table[0] (SYS0 scope) - must happen before SlabManager.init
    // because slotarray creation uses Allocator which dispatches through R7
    // Use direct register write during SYS0 bootstrap (SLB0 switch is direct, no prev chain)
    registers_set(REG_CURRENT_SCOPE, (addr)&scope_table[0]);

    // Initialize slab slot array in SYS0 reserved region (now R7 is set, Allocator works)
    if (!SlabManager.init_slab_array()) {
        fprintf(stderr, "FATAL: Unable to initialize slab slot array\n");
        abort();
    }

    // Initialize scope_table[1] for SLB0
    scope slb0 = &scope_table[1];
    slb0->scope_id = 1;
    slb0->policy = SCOPE_POLICY_DYNAMIC;
    slb0->flags = SCOPE_FLAG_SECURE;  // default SECURE flag
    // Copy name inline
    slb0->name[0] = 'S';
    slb0->name[1] = 'L';
    slb0->name[2] = 'B';
    slb0->name[3] = '0';
    slb0->name[4] = '\0';
    slb0->nodepool_base = ADDR_EMPTY;
    
    // Initialize frame fields (v0.2.3: single active frame per scope; prev-chain)
    slb0->current_frame_idx = NODE_NULL;
    slb0->current_chunk_idx = NODE_NULL;
    slb0->frame_counter = 0;
    slb0->frame_active = false;
    slb0->prev = NULL;

    // Allocate 16 pages for SLB0 (64KB total)
    const usize SLB0_PAGE_COUNT = 16;
    usize slb0_total_size = SLB0_PAGE_COUNT * SYS0_PAGE_SIZE;
    void *slb0_pages =
        mmap(NULL, slb0_total_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (slb0_pages == MAP_FAILED) {
        fprintf(stderr, "FATAL: Unable to allocate SLB0 pages\n");
        abort();
    }

    // Initialize page sentinels and link pages together
    for (usize i = 0; i < SLB0_PAGE_COUNT; i++) {
        page_sentinel ps = (page_sentinel)((sbyte *)slb0_pages + i * SYS0_PAGE_SIZE);
        ps->scope_id = 1;  // SLB0
        ps->page_index = (sbyte)i;
        ps->flags = 0;
        ps->bump_offset = sizeof(sc_page_sentinel);  // 32 bytes
        ps->_pad[0] = 0;
        ps->_pad[1] = 0;
        ps->_pad[2] = 0;
        ps->_pad[3] = 0;
        ps->_pad[4] = 0;
        ps->_pad[5] = 0;
        ps->free_list_head = 0;  // No free blocks initially
        ps->alloc_count = 0;     // No allocations yet

        // Link to next page (last page has 0)
        if (i < SLB0_PAGE_COUNT - 1) {
            ps->next_page_off = (addr)((sbyte *)slb0_pages + (i + 1) * SYS0_PAGE_SIZE);
        } else {
            ps->next_page_off = 0;  // Last page
        }
    }

    // Set scope page pointers
    slb0->first_page_off = (addr)slb0_pages;
    slb0->current_page_off = (addr)slb0_pages;  // Start at first page
    slb0->page_count = SLB0_PAGE_COUNT;

    // Register SLB0 page-0 in slab_table[1]
    SlabManager.set_slab_entry(1, (addr)slb0_pages);

    // Initialize per-scope NodePool for SLB0 (Phase 7)
    // NodePool uses mmap() so must happen after SYS0 bootstrap
    int btree_init_result = nodepool_init(slb0);
    if (btree_init_result != OK) {
        fprintf(stderr, "FATAL: Unable to initialize NodePool for SLB0 scope\n");
        abort();
    }

    // Populate skip list with initial 16 pages (Phase 7.6 - MTIS Tier 1)
    for (usize i = 0; i < SLB0_PAGE_COUNT; i++) {
        addr page_addr = (addr)((sbyte *)slb0_pages + i * SYS0_PAGE_SIZE);
        uint16_t page_idx = nodepool_alloc_page_node(slb0);
        if (page_idx == PAGE_NODE_NULL) {
            fprintf(stderr, "FATAL: Unable to allocate page_node for SLB0 page %zu\n", i);
            abort();
        }

        page_node *pn = nodepool_get_page_node(slb0, page_idx);
        if (pn == NULL) {
            fprintf(stderr, "FATAL: Unable to get page_node for index %u\n", page_idx);
            abort();
        }

        pn->page_base = page_addr;
        pn->btree_root = NODE_NULL;  // B-tree starts empty (Phase 7.7)
        pn->block_count = 0;
        for (int lvl = 0; lvl < SKIP_LIST_MAX_LEVEL; lvl++) {
            pn->forward[lvl] = PAGE_NODE_NULL;
        }

        if (skiplist_insert(slb0, page_addr, page_idx) != OK) {
            fprintf(stderr, "FATAL: Unable to insert page %zu into skip list\n", i);
            abort();
        }
    }

    // B-Tree starts empty (slb0_btree_root = NODE_NULL)
    // As blocks are freed via slb0_dispose(), they populate the tree
    // Future allocations will search tree for best-fit blocks

    // Mark bootstrap complete and switch to SLB0 as default scope
    // WARNING: After this point, sys0_alloc() will abort if called
    extern bool __bootstrap_complete;
    __bootstrap_complete = true;
    // Use direct register write — prev chain starts clean; SLB0->prev stays NULL
    registers_set(REG_CURRENT_SCOPE, (addr)slb0);

    //
}

// Bootstrap completion flag (checked by memory_state and sys0_alloc)
bool __bootstrap_complete = false;
__attribute__((destructor)) void shutdown_memory_system(void) {
    // Release SLB0 pages back to OS
    scope slb0 = get_scope_table_entry(1);
    if (slb0 != NULL && slb0->first_page_off != 0) {
        const usize SLB0_INITIAL_PAGE_COUNT = 16;
        void *slb0_initial_base = (void *)slb0->first_page_off;

        // Find page 17 (first dynamic page) BEFORE unmapping initial block
        page_sentinel dynamic_page = NULL;
        if (slb0->page_count > SLB0_INITIAL_PAGE_COUNT) {
            page_sentinel page = (page_sentinel)slb0->first_page_off;
            usize page_idx = 0;

            // Traverse to page 16 to get its next_page_off (page 17)
            while (page != NULL && page_idx < SLB0_INITIAL_PAGE_COUNT) {
                if (page->next_page_off == 0) {
                    break;
                }
                if (page_idx == SLB0_INITIAL_PAGE_COUNT - 1) {
                    // At page 16, save pointer to page 17
                    dynamic_page = (page_sentinel)page->next_page_off;
                    break;
                }
                page = (page_sentinel)page->next_page_off;
                page_idx++;
            }
        }

        // Now unmap initial 16-page block
        munmap(slb0_initial_base, SLB0_INITIAL_PAGE_COUNT * SYS0_PAGE_SIZE);

        // Unmap any dynamically allocated pages (beyond initial 16)
        if (dynamic_page != NULL) {
            page_sentinel page = dynamic_page;
            while (page != NULL) {
                page_sentinel next =
                    (page->next_page_off != 0) ? (page_sentinel)page->next_page_off : NULL;
                munmap(page, SYS0_PAGE_SIZE);
                page = next;
            }
        }

        slb0->first_page_off = 0;
        slb0->current_page_off = 0;
        slb0->page_count = 0;
        SlabManager.set_slab_entry(1, ADDR_EMPTY);
    }
    // SYS0 is static - no cleanup needed
    sys0 = NULL;
}
#endif
