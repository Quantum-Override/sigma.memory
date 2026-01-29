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
block_header memory_get_first_header(void);
static object memory_alloc_for_scope(void *scope_ptr, usize size);
static void memory_dispose_for_scope(void *scope_ptr, object ptr);
static scope get_scope_table_entry(usize index);

static void *sys0 = NULL;
static sbyte sys_page[SYS0_PAGE_SIZE] __attribute__((aligned(SYS0_PAGE_SIZE)));
// Scope table lives in SYS0 data area after header block
// scope_table[0]=SYS0, scope_table[1]=SLB0, [2-15]=user arenas
static scope scope_table = NULL;  // Points into SYS0 data area

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
                hdr->next_off = (uint)(new_hdr_off - (addr)sys_page);

                // Create new free block header
                block_header new_hdr = (block_header)new_hdr_off;
                new_hdr->next_off = 0;  // Will be last block
                new_hdr->size = (uint)(free_size - aligned - sizeof(sc_blk_header));
                new_hdr->flags = BLK_FLAG_FREE | BLK_FLAG_LAST | BLK_FLAG_FOOT;

                ptr = (object)payload_off;
            } else {
                // === NO-SPLIT PATH ===
                hdr->next_off = 0;  // No next block
                hdr->size = (uint)free_size;
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

// SYS0 disposal (mark block as free)
static void sys0_dispose(object ptr) {
    (void)ptr;  // TODO: implement - mark block FREE, coalesce adjacent
}

// Helper: Find page containing pointer (returns NULL if not in any SLB0 page)
static page_sentinel slb0_find_page(object ptr) {
    scope slb0 = get_scope_table_entry(1);
    if (slb0 == NULL || slb0->first_page_off == 0) {
        return NULL;
    }

    addr ptr_addr = (addr)ptr;
    page_sentinel page = (page_sentinel)slb0->first_page_off;

    while (page != NULL) {
        addr page_start = (addr)page;
        addr page_end = page_start + SYS0_PAGE_SIZE;

        if (ptr_addr >= page_start && ptr_addr < page_end) {
            return page;
        }

        if (page->next_page_off == 0) {
            break;
        }
        page = (page_sentinel)page->next_page_off;
    }

    return NULL;
}

// Helper: Try to allocate from free list (max 3 attempts)
static object slb0_alloc_from_free_list(page_sentinel page, usize aligned_size) {
    if (page->free_list_head == 0) {
        return NULL;
    }

    free_block prev = NULL;
    free_block current = (free_block)page->free_list_head;
    int attempts = 0;
    const int MAX_ATTEMPTS = 3;

    while (current != NULL && attempts < MAX_ATTEMPTS) {
        if (current->size >= aligned_size) {
            // Found a fit - remove from list
            if (prev == NULL) {
                page->free_list_head = current->next_free_off;
            } else {
                prev->next_free_off = current->next_free_off;
            }
            page->alloc_count++;
            return (object)current;
        }

        prev = current;
        if (current->next_free_off == 0) {
            break;
        }
        current = (free_block)current->next_free_off;
        attempts++;
    }

    return NULL;  // No fit found in 3 attempts
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

// SLB0 allocation (free list search + bump pointer)
static object slb0_alloc(usize size) {
    object ptr = NULL;
    scope slb0 = get_scope_table_entry(1);
    if (slb0 == NULL || size == 0) {
        goto exit;
    }

    // Enforce minimum allocation size and alignment
    usize aligned_size = size < SLB0_MIN_ALLOC ? SLB0_MIN_ALLOC : size;
    aligned_size = (aligned_size + (kAlign - 1)) & ~(kAlign - 1);

    // Start from current page
    page_sentinel page = (page_sentinel)slb0->current_page_off;
    if (page == NULL) {
        page = (page_sentinel)slb0->first_page_off;
    }

    // Try each page in chain
    while (page != NULL) {
        // Try free list first (max 3 attempts)
        ptr = slb0_alloc_from_free_list(page, aligned_size);
        if (ptr != NULL) {
            slb0->current_page_off = (addr)page;
            goto exit;
        }

        // Try bump allocation
        ptr = slb0_bump_alloc(page, aligned_size);
        if (ptr != NULL) {
            slb0->current_page_off = (addr)page;
            goto exit;
        }

        // Move to next page
        if (page->next_page_off == 0) {
            break;  // No more pages (FIXED policy would return NULL here)
        }
        page = (page_sentinel)page->next_page_off;
    }

    // TODO: DYNAMIC policy - allocate new page and chain it

exit:
    return ptr;
}

// Helper: Unlink and release a page from the chain
static void slb0_release_page(page_sentinel page) {
    scope slb0 = get_scope_table_entry(1);
    if (slb0 == NULL) {
        return;
    }

    // Find the page before this one
    page_sentinel prev = NULL;
    page_sentinel current = (page_sentinel)slb0->first_page_off;

    while (current != NULL && current != page) {
        prev = current;
        if (current->next_page_off == 0) {
            return;  // Page not found in chain
        }
        current = (page_sentinel)current->next_page_off;
    }

    if (current != page) {
        return;  // Page not in chain
    }

    // Unlink the page
    if (prev == NULL) {
        // Releasing first page
        slb0->first_page_off = page->next_page_off;
        if (slb0->first_page_off != 0) {
            SlabManager.set_slab_slot(1, slb0->first_page_off);
        } else {
            SlabManager.set_slab_slot(1, ADDR_EMPTY);
        }
    } else {
        prev->next_page_off = page->next_page_off;
    }

    // Update current_page_off if needed
    if (slb0->current_page_off == (addr)page) {
        slb0->current_page_off = slb0->first_page_off;
    }

    slb0->page_count--;

    // Release the page back to OS
    munmap(page, SYS0_PAGE_SIZE);
}

// SLB0 disposal (page-level tracking with free list)
static void slb0_dispose(object ptr) {
    if (ptr == NULL) {
        return;
    }

    // Find the page containing this pointer
    page_sentinel page = slb0_find_page(ptr);
    if (page == NULL) {
        return;  // Not in SLB0
    }

    // Calculate block size (we don't store it, so use minimum)
    // TODO: Store size in allocation for proper free list sizing
    free_block fb = (free_block)ptr;
    fb->size = SLB0_MIN_ALLOC;  // Conservative estimate
    fb->next_free_off = page->free_list_head;
    page->free_list_head = (addr)fb;

    // Decrement allocation count
    if (page->alloc_count > 0) {
        page->alloc_count--;
    }

    // Release page if empty
    if (page->alloc_count == 0 && page->bump_offset > sizeof(sc_page_sentinel)) {
        slb0_release_page(page);
    }
}

// Get scope table entry by index
static scope get_scope_table_entry(usize index) {
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
// Set current scope pointer in R7
bool memory_set_current_scope(void *scope_ptr) {
    bool success = false;
    if (scope_ptr == NULL) {
        goto exit;
    }
    registers_set(REG_CURRENT_SCOPE, (addr)scope_ptr);  // Set R7 to new scope pointer
    success = true;

exit:
    return success;
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
            // TODO: implement user arena allocation
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
            // TODO: implement user arena disposal
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
            // TODO: implement user arena allocation
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
            // TODO: implement user arena disposal
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
    if (((addr)ftr % _Alignof(uint)) == 0) {
        state |= MEM_STATE_ALIGN_FOOTER;
    }

    // Check if user memory (SLB0) is ready
    scope cur_scope = (scope)memory_get_current_scope();
    if (cur_scope != NULL && cur_scope->scope_id == 1) {
        state |= MEM_STATE_USER_READY;
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
    return (addr)(sys_page + SYS0_SLOTS_OFFSET);
}
// Get slab slot end address
addr memory_get_slots_end(void) {
    return (addr)(sys_page + SYS0_SLOTS_OFFSET + SYS0_SLOTS_SIZE);
}
#endif

#if 1  // Region: Allocator Interface
const sc_allocator_i Allocator = {
    .alloc = memory_alloc,
    .dispose = memory_dispose,
    .Scope =
        {
            .current = memory_get_current_scope,
            .set = memory_set_current_scope,
            .config = memory_get_scope_config,
            .alloc = memory_alloc_for_scope,
            .dispose = memory_dispose_for_scope,
        },
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
    sys0_entry->reserved[0] = 0;
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
    scope_table = (scope)sys0_alloc(SCOPE_TABLE_COUNT * SCOPE_ENTRY_SIZE);
    if (scope_table == NULL) {
        fprintf(stderr, "FATAL: Unable to allocate scope table\n");
        abort();
    }

    // Initialize scope_table[0] for SYS0
    init_scope_table_sys0();

    // Set R7 to point to scope_table[0] (SYS0 scope) - must happen before SlabManager.init
    // because slotarray creation uses Allocator which dispatches through R7
    memory_set_current_scope(&scope_table[0]);

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
    slb0->reserved[0] = 0;

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

    // Register SLB0 page-0 in slab_slots[1]
    SlabManager.set_slab_slot(1, (addr)slb0_pages);

    // Set R7 to point to SLB0 (make it the current scope)
    memory_set_current_scope(slb0);

    //
}
__attribute__((destructor)) void shutdown_memory_system(void) {
    // Release SLB0 pages back to OS
    scope slb0 = get_scope_table_entry(1);
    if (slb0 != NULL && slb0->first_page_off != 0) {
        // Walk the page chain and release each page
        page_sentinel page = (page_sentinel)slb0->first_page_off;
        while (page != NULL) {
            page_sentinel next =
                (page->next_page_off != 0) ? (page_sentinel)page->next_page_off : NULL;
            munmap(page, SYS0_PAGE_SIZE);
            page = next;
        }
        slb0->first_page_off = 0;
        slb0->current_page_off = 0;
        slb0->page_count = 0;
        SlabManager.set_slab_slot(1, ADDR_EMPTY);
    }
    // SYS0 is static - no cleanup needed
    sys0 = NULL;
}
#endif
