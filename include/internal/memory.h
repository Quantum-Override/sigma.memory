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
#include <stdint.h>  // Explicit include for uint32_t, etc.

// layout constants
#define kAlign 16                                     // 16-byte alignment
#define SYS0_PAGE_SIZE 8192                           // 8 KB system page size (v0.2.0)
#define BLK_END 0xDEADC0DE                            // end of block marker
#define SYS0_REGISTERS_OFFSET 0                       // offset of registers region
#define SYS0_REGISTERS_SIZE (8 * sizeof(addr))        // 64 bytes (R0-R7)
#define SYS0_SLAB_TABLE_OFFSET (SYS0_REGISTERS_SIZE)  // 64 bytes
#define SYS0_SLAB_TABLE_SIZE 128                      // 128 bytes (SlabTable[16], 8 bytes each)
#define SYS0_STACK_OFFSET (SYS0_SLAB_TABLE_OFFSET + SYS0_SLAB_TABLE_SIZE)  // 192
#define SYS0_STACK_SIZE 128  // 128 bytes (generic Stack[16], 8 bytes each)
#define SYS0_NODE_TABLE_OFFSET (SYS0_STACK_OFFSET + SYS0_STACK_SIZE)  // 320
#define SYS0_NODE_TABLE_SIZE 30  // 30 bytes (NodeTable[15], 2 bytes each)
#define SYS0_NODE_STACK_OFFSET (SYS0_NODE_TABLE_OFFSET + SYS0_NODE_TABLE_SIZE)  // 350
#define SYS0_NODE_STACK_SIZE 128                 // 128 bytes (NodeStack depth + 15 slots)
#define SYS0_RESERVED_SIZE 1536                  // 1536 bytes (478-1536 unused: 1058 bytes)
#define FIRST_BLOCK_OFFSET (SYS0_RESERVED_SIZE)  // 1536

#define LAST_FOOTER_OFFSET (SYS0_PAGE_SIZE - sizeof(sc_blk_footer))  // 8184
// Scope table layout (in SYS0 data area, not reserved)
#define SCOPE_TABLE_COUNT 16  // 16 scope entries
#define SCOPE_ENTRY_SIZE 96   // Each scope entry: sizeof(sc_scope) — keep in sync

#if 1  // Region: Two Phase Memory Initilization
/*
 * PHASE 1: Bootstrap (during constructor, before main())
 * -------------------------------------------------------
 * - RESERVED region (0-1535): Static geometry structures (registers, slab table, NodeTable,
 * NodeStack)
 * - DAT region (1536-8191): Bootstrap-time dynamic allocator (sys0_alloc with headers)
 *   - R7 points to scope_table[0] (SYS0 scope)
 *   - Allocator.alloc() → sys0_alloc() for permanent bootstrap objects
 *   - All allocations are PERMANENT (no dispose during bootstrap)
 *   - After init: DAT frozen, MEM_STATE_BOOTSTRAP_COMPLETE set
 *
 * PHASE 2: Runtime (after constructor completes)
 * -------------------------------------------------------
 * - R7 switches to scope_table[1] (SLB0 becomes default scope)
 * - Allocator.alloc() → slb0_alloc() for dynamic runtime allocations
 * - sys0_alloc() disabled (asserts if called post-bootstrap)
 * - All user allocations in SLB0+ pages (external to SYS0)
 *
 * WHY SYS0 SCOPE EXISTS:
 * - Bootstrap needs dynamic allocation (scope_table size, future objects)
 * - Header overhead acceptable for bootstrap (permanent, no fragmentation)
 * - After bootstrap: DAT = static data + legacy headers (wasted space OK)
 */
#endif

#if 1  // Region: SYS0 Reserved Structures
// Memory state flags
enum {
    MEM_STATE_ALIGN_SYS0 = 1u << 0,          // sys0 page is kAlign aligned
    MEM_STATE_ALIGN_HEADER = 1u << 1,        // header size is kAlign multiple
    MEM_STATE_ALIGN_FOOTER = 1u << 2,        // footer placement is naturally aligned
    MEM_STATE_BOOTSTRAP_COMPLETE = 1u << 3,  // bootstrap phase complete (DAT frozen)
    // bits 4-5 reserved for future use
    MEM_STATE_USER_READY = 1u << 6,  // user memory system is ready
    MEM_STATE_READY = 1u << 7,       // memory system is ready
};
// Virtual registers (R0-R7) cached in SYS0
typedef struct sc_registers {
    addr R0;     // SYS0 base address (R0: enables relative offset resolution)
    addr R1;     // Reserved (will cache NodePool base in v0.2.0)
    addr R2;     // Operation result register (convention: B-Tree ops return via R2)
    addr R3;     // Reserved
    addr R4;     // Reserved
    addr R5;     // Reserved
    addr R6;     // Reserved (will cache parent scope in v0.2.0)
    addr R7;     // Current scope pointer (R7: enables O(1) scope lookup)
} sc_registers;  // size 64 bytes
typedef struct sc_registers *registers;

#endif

#if 1  // Region: B-Tree Node Definitions (v0.2.0)
// Node index type (16-bit index into NodePool)
typedef uint16_t node_idx;
#define NODE_NULL ((node_idx)0)  // Null node index

// B-Tree node structure (24 bytes - cache-friendly, 3 nodes per cache line)
typedef struct sc_node {
    addr start;          // 8: allocation start address
    uint32_t length;     // 4: actual size in bytes (up to 4GB)
    uint16_t left_idx;   // 2: left child index (NODE_NULL if none)
    uint16_t right_idx;  // 2: right child index (NODE_NULL if none)

    // Info field (2 bytes):
    // Bits 0-7:   log2(max_free_size) - represents 2^0 to 2^255 bytes
    // Bit 8:      direction (0=left, 1=right subtree has max)
    // Bit 9:      FREE_FLAG - this node represents a free block
    // Bit 10:     FRAME_NODE_FLAG - this node is a frame chunk
    // Bits 11-15: reserved for future use
    uint16_t info;  // 2: allocation info and flags

    // _reserved[6] used for frame extensions:
    // - FRAME_NODE_FLAG: frame_data.{frame_offset, next_chunk_idx, frame_id}
    // - FRAME_LARGE_FLAG: first 2 bytes = next_large_alloc_idx (linked list)
    union {
        struct {
            uint16_t frame_offset;    // 2: current bump position in chunk (frame use)
            uint16_t next_chunk_idx;  // 2: next chunk in chain (frame use)
            uint16_t frame_id;        // 2: frame identifier
        } frame_data;
        struct {
            uint16_t next_large_alloc;  // 2: next large allocation in frame (FRAME_LARGE_FLAG)
            uint8_t _pad[4];            // 4: unused
        } large_alloc_data;
        uint8_t _reserved[6];  // 6: raw reserved space
    };
} sc_node;  // Total: 24 bytes - cache-line friendly (3 nodes per 64B line)
typedef struct sc_node *btree_node;

// Bit masks for info field
#define NODE_SIZE_MASK 0x00FF      // Bits 0-7: log2 size
#define NODE_DIRECTION_BIT 0x0100  // Bit 8: direction
#define NODE_FREE_FLAG 0x0200      // Bit 9: free flag
#define FRAME_NODE_FLAG 0x0400     // Bit 10: frame chunk flag (bump allocated)
#define FRAME_LARGE_FLAG 0x0800    // Bit 11: frame large allocation (>4KB, B-tree allocated)
#define NODE_RESERVED_MASK 0xF000  // Bits 12-15: reserved

// Skip list constants
#define SKIP_LIST_MAX_LEVEL 4  // 4 levels (0-3) - sufficient for 256+ pages
#define SKIP_LIST_P 0.5        // 50% probability for level promotion

// Frame constants
#define FRAME_CHUNK_SIZE 4096  // 4KB chunks for frame allocations
#define MAX_FRAME_DEPTH 1      // Maximum depth per scope: single active frame

// NodePool header structure (40 bytes) - Phase 7 two-tier architecture
// Initial size: 2KB (small start, grows dynamically via mremap)
// Growth pattern: 2KB → 4KB → 8KB → 16KB → 32KB (doubles each time)
typedef struct nodepool_header {
    usize capacity;            // 8: Total mmap'd size (starts at 2KB, grows dynamically)
    usize page_count;          // 8: Number of pages in skip list
    usize page_alloc_offset;   // 8: Next free page_node slot (grows up from header)
    usize btree_alloc_offset;  // 8: Next free btree_node slot (grows down from top)
    uint16_t skip_list_head;    // 2: Index of first page_node in skip list
    uint16_t btree_free_head;   // 2: Head of recycled btree_node free list (NODE_NULL = empty)
    uint16_t _reserved[6];      // 12: Reserved for future use
} nodepool_header;              // Total: 40 bytes

// Page directory node (20 bytes) - Skip list entry for page tracking
typedef struct page_node {
    addr page_base;                         // 8: Base address of 8KB page
    uint16_t forward[SKIP_LIST_MAX_LEVEL];  // 8: Skip list forward pointers (4 levels)
    uint16_t btree_root;                    // 2: Root of this page's B-tree (NODE_NULL if empty)
    uint16_t block_count;                   // 2: Number of blocks allocated in this page
} page_node;                                // Total: 20 bytes

#define PAGE_NODE_NULL ((uint16_t)0)  // Null page_node index

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
    uint32_t next_off;  // Offset from sys_page base to next header
    uint32_t size;      // Usable size of the payload block in bytes
    sbyte flags;        // Bitfield: FREE, LAST, HAS_FOOTER, reserved
    sbyte _pad[7];      // Padding for alignment
} sc_blk_header;        // size 16 bytes
typedef struct sc_blk_header *block_header;
typedef struct sc_blk_footer {
    uint32_t magic;  // #DEADC0DE marker
    uint32_t size;   // Copy of the header.size for backward traversal
} sc_blk_footer;     // size 8 bytes
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
// DEPRECATED (Phase 7): Will be removed in favor of page_node in NodePool
// Currently used by Phase 6 code paths, to be eliminated by Task 7-8
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

// Frame state structure for nesting support
typedef struct sc_frame_state {
    uint16_t frame_id;           // Frame identifier
    uint16_t head_chunk_idx;     // First chunk in chain (bump allocated)
    uint16_t large_allocs_head;  // First large allocation (>4KB, B-tree allocated)
    uint16_t _pad;               // Alignment padding
    uint32_t total_allocated;    // Bytes allocated in frame
} sc_frame_state;

// Unified scope table entry (64 bytes base + frame extensions)
// Layout: scope_table[0]=SYS0, scope_table[1]=SLB0, [2-15]=user arenas
// The slab_table[i] array parallels scope_table[i] with page-0 addresses
typedef struct sc_scope {
    usize scope_id;         // 8: Unique ID (matches index in scope_table)
    sbyte policy;           // 1: SCOPE_POLICY_* (immutable after creation)
    sbyte flags;            // 1: SCOPE_FLAG_* bitmask (mutable)
    sbyte _pad[6];          // 6: Alignment padding
    addr first_page_off;    // 8: Offset to first page's sentinel
    addr current_page_off;  // 8: Offset to current (last active) page
    usize page_count;       // 8: Number of pages in chain
    char name[16];          // 16: Inline scope name (null-terminated)
    addr nodepool_base;     // 8: Base address of per-scope NodePool mmap

    // Frame support (v0.2.3: single active frame per scope; prev-chain replaces R7 stack)
    uint16_t current_frame_idx;   // Head chunk node index (NODE_NULL when no frame active)
    uint16_t current_chunk_idx;   // Current bump chunk (may differ from head after chaining)
    uint16_t frame_counter;       // Monotonic frame ID generator (never reset)
    bool     frame_active;        // True when a frame is open on this scope
    uint8_t  _frame_pad;          // Alignment padding
    scope    prev;                // Previous scope in activation chain (NULL = root)
    sc_frame_state active_frame;  // Current frame state (valid only when frame_active)
} sc_scope;  // Total: 96 bytes (verified by _Static_assert below)
_Static_assert(sizeof(sc_scope) == SCOPE_ENTRY_SIZE,
               "SCOPE_ENTRY_SIZE must equal sizeof(sc_scope) — update the define");
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
    scope (*get_scope)(usize index);  // For testing: access scope table
} sc_memory_i;
extern const sc_memory_i Memory;
#endif