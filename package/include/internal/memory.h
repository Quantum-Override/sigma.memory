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
 * File: internal/memory.h
 * Description: SigmaCore memory management implementation
 */

#pragma once

#include "config.h"
// ----------------
#include <sigma.core/types.h>

// layout constants
#define kAlign 16                                // 16-byte alignment
#define SYS0_PAGE_SIZE 4096                      // 4 KB system page size
#define BLK_END 0xDEADC0DE                       // end of block marker
#define SYS0_REGISTERS_OFFSET 0                  // offset of registers region
#define SYS0_REGISTERS_SIZE (8 * sizeof(addr))   // 64 bytes
#define SYS0_SLOTS_OFFSET (SYS0_REGISTERS_SIZE)  // 64 bytes
#define SYS0_SLOTS_SIZE 128                      // 128 bytes (16 slots of 8 bytes each)
#define SYS0_SLOTS_END (SYS0_SLOTS_OFFSET + SYS0_SLOTS_SIZE)         // 192
#define SYS0_RESERVED_SIZE 256                                       // Power-of-2 reserved region
#define FIRST_BLOCK_OFFSET (SYS0_RESERVED_SIZE)                      // 256
#define LAST_FOOTER_OFFSET (SYS0_PAGE_SIZE - sizeof(sc_blk_footer))  // 4088
// Scope table layout (in SYS0 data area, not reserved)
#define SCOPE_TABLE_COUNT 16  // 16 scope entries
#define SCOPE_ENTRY_SIZE 64   // Each scope entry is 64 bytes

#if 1  // Region: SYS0 Reserved Structures
// Memory state flags
enum {
    MEM_STATE_ALIGN_SYS0 = 1u << 0,    // sys0 page is kAlign aligned
    MEM_STATE_ALIGN_HEADER = 1u << 1,  // header size is kAlign multiple
    MEM_STATE_ALIGN_FOOTER = 1u << 2,  // footer placement is naturally aligned
    // bits 3-5 reserved for future use
    MEM_STATE_USER_READY = 1u << 6,  // user memory system is ready
    MEM_STATE_READY = 1u << 7,       // memory system is ready
};
// Virtual registers (R0-R7) cached in SYS0
typedef struct sc_registers {
    addr R0;     // SYS0 base address (R0: enables relative offset resolution)
    addr R1;     // Reserved
    addr R2;     // Reserved
    addr R3;     // Reserved
    addr R4;     // Reserved
    addr R5;     // Reserved
    addr R6;     // Reserved
    addr R7;     // Current scope pointer (R7: enables O(1) scope lookup)
} sc_registers;  // size 64 bytes
typedef struct sc_registers *registers;

#endif

#if 1  // Region: Memory Block Definitions
// Meta-block flags
enum {
    BLK_FLAG_FREE = 1u << 0,
    BLK_FLAG_LAST = 1u << 1,
    BLK_FLAG_FOOT = 1u << 2,
};
// Block header/footer structures
typedef struct sc_blk_header {
    uint next_off;  // Offset from sys_page base to next header
    uint size;      // Usable size of the payload block in bytes
    sbyte flags;    // Bitfield: FREE, LAST, HAS_FOOTER, reserved
    sbyte _pad[7];  // Padding for alignment
} sc_blk_header;    // size 16 bytes
typedef struct sc_blk_header *block_header;
typedef struct sc_blk_footer {
    uint magic;   // #DEADC0DE marker
    uint size;    // Copy of the header.size for backward traversal
} sc_blk_footer;  // size 8 bytes
typedef struct sc_blk_footer *block_footer;
#endif

#if 1  // Region: Scope & Page Definitions
// Minimum allocation size (must hold sc_free_block on free)
#define SLB0_MIN_ALLOC 16

// Free block node (stored in-place in freed memory)
typedef struct sc_free_block {
    addr next_free_off;  // Address of next free block (0 = end of list)
    usize size;          // Size of this free block
} sc_free_block;         // size 16 bytes
typedef struct sc_free_block *free_block;

// Page sentinel (metadata at start of each SLB page)
typedef struct sc_page_sentinel {
    addr next_page_off;   // Offset to next page in chain (0 = last page)
    addr bump_offset;     // Current bump pointer within this page
    usize scope_id;       // Owning scope ID
    sbyte flags;          // Page-level flags (reserved)
    sbyte page_index;     // Position in scope's page chain (0 = first)
    sbyte _pad[6];        // Alignment to 32 bytes
    addr free_list_head;  // Address of first free block (0 = none)
    usize alloc_count;    // Number of live allocations on this page
} sc_page_sentinel;       // size 32 bytes
typedef struct sc_page_sentinel *page_sentinel;

// Forward declaration for scope pointer
typedef struct sc_scope *scope;

// Unified scope table entry (64 bytes - fits 16 entries in 1KB)
// Layout: scope_table[0]=SYS0, scope_table[1]=SLB0, [2-15]=user arenas
// The slab_slots[i] array parallels scope_table[i] with page-0 addresses
typedef struct sc_scope {
    usize scope_id;         // 8: Unique ID (matches index in scope_table)
    sbyte policy;           // 1: SCOPE_POLICY_* (immutable after creation)
    sbyte flags;            // 1: SCOPE_FLAG_* bitmask (mutable)
    sbyte _pad[6];          // 6: Alignment padding
    addr first_page_off;    // 8: Offset to first page's sentinel
    addr current_page_off;  // 8: Offset to current (last active) page
    usize page_count;       // 8: Number of pages in chain
    char name[16];          // 16: Inline scope name (null-terminated)
    addr reserved[1];       // 8: Reserved for future use
} sc_scope;                 // Total: 64 bytes
#endif

#if 1  // Region: Internal Memory Interface
struct sc_slab_manager_i;
extern const struct sc_slab_manager_i SlabManager;

// Note: sys0_alloc/dispose are now private (static) methods in memory.c
// Tests should use Allocator.alloc/dispose or Allocator.Scope methods with explicit scope instead
typedef struct sc_memory_i {
    usize (*sys0_size)(void);
    sbyte (*state)(void);
    block_header (*get_first_header)(void);
    block_footer (*get_last_footer)(void);
    addr (*get_sys0_base)(void);
    const struct sc_slab_manager_i *SlabManager;
    addr (*get_slots_base)(void);
    addr (*get_slots_end)(void);
} sc_memory_i;
extern const sc_memory_i Memory;
#endif