# SigmaCore Memory System - Architecture & Reference Guide

**Version:** 0.1.0  
**Date:** January 29, 2026  
**Status:** Implemented & Tested (30 tests passing)

---

## 1. Overview

The SigmaCore memory management system implements a **dual-allocator architecture** with a unified scope table design:

| Component | Description | Strategy |
|-----------|-------------|----------|
| **SYS0** | Bootstrap allocator (4KB static page) | First-fit reclaiming |
| **SLB0** | Default user-space allocator (16 pages) | Bump allocation |
| **SLBn** | User-defined arenas (scope_table[2-15]) | Bump allocation |

### Key Design Decisions

- **Unified Scope Table**: All scopes (SYS0, SLB0, user arenas) stored in a fixed `scope_table[16]` allocated from SYS0
- **ID-Based Dispatch**: Allocation routes through `scope_id` switch instead of function pointers
- **Parallel Arrays**: `slab_slots[i]` parallels `scope_table[i]` with page-0 addresses
- **Slim Scope Struct**: 64 bytes per scope entry (inline name, no function pointers)

---

## 2. Memory Layout

### 2.1 SYS0 Page (4KB Static)

```
Offset      Size    Content                    
────────────────────────────────────────────────
RESERVED REGION (0-255)
────────────────────────────────────────────────
0           64      Registers R0-R7            
64          128     Slab Slots[16]             
192         64      [Unused reserved space]    
────────────────────────────────────────────────
DATA AREA (256-4095)
────────────────────────────────────────────────
256         16      First Block Header         
272         1024    scope_table[16]            
~1296       24      SlotArray wrapper struct   
...         ...     [Remaining free space]     
4088        8       Footer Marker (0xDEADC0DE) 
────────────────────────────────────────────────
Total:      4096 bytes
```

### 2.2 Layout Constants

```c
#define kAlign 16                    // 16-byte alignment
#define SYS0_PAGE_SIZE 4096          // 4 KB

// Reserved Region
#define SYS0_REGISTERS_OFFSET 0      // R0-R7 (64 bytes)
#define SYS0_REGISTERS_SIZE 64
#define SYS0_SLOTS_OFFSET 64         // Slab slots (128 bytes)
#define SYS0_SLOTS_SIZE 128
#define SYS0_SLOTS_END 192
#define SYS0_RESERVED_SIZE 256       // Power-of-2 boundary

// Data Area
#define FIRST_BLOCK_OFFSET 256
#define LAST_FOOTER_OFFSET 4088

// Scope Table
#define SCOPE_TABLE_COUNT 16
#define SCOPE_ENTRY_SIZE 64          // 16 * 64 = 1024 bytes
```

---

## 3. Core Structures

### 3.1 Registers (64 bytes)

```c
typedef struct sc_registers {
    addr R0;  // SYS0 base address (offset resolution)
    addr R1;  // Reserved
    addr R2;  // Reserved
    addr R3;  // Reserved
    addr R4;  // Reserved
    addr R5;  // Reserved
    addr R6;  // Reserved
    addr R7;  // Current scope pointer (points into scope_table)
} sc_registers;
```

**R0** enables relative offset calculations from SYS0 base.  
**R7** caches the current scope pointer for O(1) lookup.

### 3.2 Scope Entry (64 bytes)

```c
typedef struct sc_scope {
    usize scope_id;         //  8: Index in scope_table
    sbyte policy;           //  1: SCOPE_POLICY_*
    sbyte flags;            //  1: SCOPE_FLAG_* bitmask
    sbyte _pad[6];          //  6: Alignment
    addr first_page_off;    //  8: First page address
    addr current_page_off;  //  8: Current (active) page
    usize page_count;       //  8: Pages in chain
    char name[16];          // 16: Inline name
    addr reserved[1];       //  8: Reserved
} sc_scope;                 // Total: 64 bytes
```

**Scope Table Layout:**
| Index | Scope | Policy | Flags |
|-------|-------|--------|-------|
| 0 | SYS0 | RECLAIMING | PROTECTED \| PINNED |
| 1 | SLB0 | DYNAMIC | SECURE |
| 2-15 | User arenas | User-defined | User-defined |

### 3.3 Block Header (16 bytes)

```c
typedef struct sc_blk_header {
    uint next_off;   // Offset to next header (0 = last)
    uint size;       // Usable payload size
    sbyte flags;     // BLK_FLAG_FREE | LAST | FOOT
    sbyte _pad[7];   // Alignment
} sc_blk_header;
```

### 3.4 Block Footer (8 bytes)

```c
typedef struct sc_blk_footer {
    uint magic;      // 0xDEADC0DE
    uint size;       // Duplicate for backward traversal
} sc_blk_footer;
```

### 3.5 Page Sentinel (32 bytes)

```c
typedef struct sc_page_sentinel {
    addr next_page_off;   // Next page address (0 = last)
    addr bump_offset;     // Current bump pointer
    usize scope_id;       // Owning scope ID
    sbyte flags;          // Page-level flags
    sbyte page_index;     // Position in chain (0-based)
    sbyte _pad[6];        // Alignment
    addr free_list_head;  // First free block address (0 = none)
    usize alloc_count;    // Live allocations on this page
} sc_page_sentinel;       // 32 bytes
```

### 3.6 Free Block (16 bytes)

Stored in-place in freed memory for free list management:

```c
typedef struct sc_free_block {
    addr next_free_off;   // Next free block address (0 = end)
    usize size;           // Size of this free block
} sc_free_block;          // 16 bytes (SLB0_MIN_ALLOC)
```

---

## 4. Allocation Strategies

### 4.1 SYS0 (Reclaiming)

**Strategy:** First-fit with block splitting

```
┌──────────────────────────────────────────────────────┐
│ Header │ Payload (aligned to kAlign) │ [Next Header] │
└──────────────────────────────────────────────────────┘
```

- Searches free blocks for first fit
- Splits blocks if remainder > MIN_REMAINDER (32 bytes)
- Marks allocated blocks by clearing BLK_FLAG_FREE
- Footer at page end for integrity validation

**Use Cases:**
- scope_table allocation
- SlotArray wrapper struct
- System metadata objects

### 4.2 SLB0/Arena (Hybrid Bump + Free List)

**Strategy:** Bump pointer with per-page free list reuse

```
Page 0                    Page 1                    Page N
┌────────────────────┐   ┌────────────────────┐   ┌────────────────────┐
│ Sentinel (32B)     │──▶│ Sentinel (32B)     │──▶│ Sentinel (32B)     │
│ bump_offset ──────▶│   │ bump_offset ──────▶│   │ bump_offset ──────▶│
│ free_list_head ───▶│   │ free_list_head ───▶│   │ free_list_head ───▶│
│ alloc_count: N     │   │ alloc_count: M     │   │ alloc_count: K     │
│ [allocated data]   │   │ [allocated data]   │   │ [allocated data]   │
│ [free blocks]      │   │ [free blocks]      │   │ [free blocks]      │
└────────────────────┘   └────────────────────┘   └────────────────────┘
     4096 bytes               4096 bytes               4096 bytes
```

**Allocation Flow:**
1. Enforce minimum size (`SLB0_MIN_ALLOC = 16`) and kAlign alignment
2. Check free list (max 3 first-fit attempts)
3. If found → remove from list, return pointer
4. Else → bump allocate from current page
5. If page exhausted → advance to next page, repeat
6. Increment `alloc_count`

**Dispose Flow:**
1. Find owning page (address range check)
2. Add block to page's free list (prepend)
3. Decrement `alloc_count`
4. If `alloc_count == 0` → release page via `munmap()`

**Page Release:**
- All pages eligible for release (including page-0)
- Released pages unlinked from chain
- `slab_slots[1]` updated if page-0 released

---

## 5. Public API

### 5.1 Allocator Interface

```c
// Top-level (uses current scope from R7)
Allocator.alloc(size)        // Allocate from current scope
Allocator.dispose(ptr)       // Dispose to current scope

// Explicit scope operations
Allocator.Scope.current()    // Get current scope pointer
Allocator.Scope.set(scope)   // Set current scope (updates R7)
Allocator.Scope.config(scope, mask)  // Query policy/flags
Allocator.Scope.alloc(scope, size)   // Allocate from explicit scope
Allocator.Scope.dispose(scope, ptr)  // Dispose to explicit scope
```

### 5.2 Memory Interface (Internal)

```c
Memory.sys0_size()           // Returns 4096
Memory.state()               // Returns MEM_STATE_* flags
Memory.get_first_header()    // First block header in SYS0
Memory.get_last_footer()     // Footer marker
Memory.get_sys0_base()       // R0 value (SYS0 base address)
Memory.get_slots_base()      // Slab slots array start
Memory.get_slots_end()       // Slab slots array end
Memory.SlabManager->...      // Slab slot operations
```

### 5.3 SlabManager Interface (Internal)

```c
SlabManager.init_slab_array()           // Initialize slotarray wrapper
SlabManager.get_slab_slot(index)        // Get page-0 address (uses PArray.get)
SlabManager.set_slab_slot(index, addr)  // Set page-0 address (uses PArray.set)
```

> **Implementation Note:** The slab slot array uses `PArray.get()` and `PArray.set()` 
> for consistency. A `SlotArray.set_at()` method would be a useful addition to sigma.collections.

---

## 6. Scope Policies & Flags

### 6.1 Policies (Immutable)

| Policy | Value | Description |
|--------|-------|-------------|
| `SCOPE_POLICY_RECLAIMING` | 0 | SYS0 only; first-fit with block reuse |
| `SCOPE_POLICY_DYNAMIC` | 1 | Auto-grows by chaining pages |
| `SCOPE_POLICY_FIXED` | 2 | Pre-allocated; NULL when exhausted |

### 6.2 Flags (Mutable)

| Flag | Bit | Description |
|------|-----|-------------|
| `SCOPE_FLAG_PROTECTED` | 0x01 | Blocks destroy/reset |
| `SCOPE_FLAG_PINNED` | 0x02 | Blocks set_current and frame ops |
| `SCOPE_FLAG_SECURE` | 0x04 | Blocks cross-scope move/copy |

---

## 7. State Flags

```c
enum {
    MEM_STATE_ALIGN_SYS0   = 0x01,  // sys_page is kAlign-aligned
    MEM_STATE_ALIGN_HEADER = 0x02,  // header size is kAlign multiple
    MEM_STATE_ALIGN_FOOTER = 0x04,  // footer is naturally aligned
    MEM_STATE_USER_READY   = 0x40,  // SLB0 initialized
    MEM_STATE_READY        = 0x80,  // Memory system ready
};
```

---

## 8. Initialization Sequence

```
1. Verify SYS0_PAGE_SIZE is power-of-2
2. Set sys0 = sys_page (mark ready)
3. Initialize registers R0-R7 to 0
4. Set R0 = sys_page base address
5. Initialize first block header (offset 256)
6. Initialize footer marker (offset 4088)
7. Allocate scope_table[16] from SYS0 (1024 bytes)
8. Initialize scope_table[0] for SYS0
9. Set R7 = &scope_table[0] (current scope)
10. Initialize SlabManager (slab_slots wrapper)

#ifndef TEST_BOOTSTRAP_ONLY
11. Initialize scope_table[1] for SLB0
12. Allocate 16 pages via mmap (64KB)
13. Initialize page sentinels and link chain
14. Set slab_slots[1] = SLB0 page-0 address
15. Set R7 = &scope_table[1] (SLB0 is current)
#endif
```

---

## 9. Dispatch Mechanism

Allocation dispatch uses `scope_id` switch instead of function pointers:

```c
object memory_alloc(usize size) {
    scope current = memory_get_current_scope();
    switch (current->scope_id) {
        case 0:  return sys0_alloc(size);   // SYS0
        case 1:  return slb0_alloc(size);   // SLB0
        default: /* user arena */ break;
    }
}
```

**Benefits:**
- Smaller scope struct (64 bytes vs ~120)
- No function pointer indirection
- Compile-time optimization potential
- Simpler initialization

---

## 10. Files Reference

| File | Purpose |
|------|---------|
| `include/memory.h` | Public API (Allocator interface) |
| `include/internal/memory.h` | Internal structures and Memory interface |
| `include/internal/slab_manager.h` | SlabManager interface |
| `src/memory.c` | Implementation (allocation, init) |
| `src/slab_manager.c` | Slab slot array management |
| `test/test_bootstrap.c` | SYS0 bootstrap tests (10 tests) |
| `test/test_slab0.c` | SLB0 tests (20 tests) |

---

## 11. Test Coverage

### Bootstrap Tests (SYS0)
1. Memory state ready
2. SYS0 size is 4KB
3. Page alignment (kAlign)
4. Header alignment
5. Footer alignment
6. Footer magic marker
7. First block offset = 256
8. Last footer offset = 4088
9. Initial block is FREE | LAST | FOOT
10. Slab slot allocation

### SLB0 Tests (20 total)

**Initialization (10):**
1. Scope exists (scope_id = 1)
2. Has DYNAMIC policy
3. Has SECURE flag by default
4. Has 16 pages initial
5. Page chain initialized
6. First page sentinel initialized
7. Bump offset starts at 32
8. slab_slots[1] has page-0 address
9. Is current scope (R7)
10. MEM_STATE_USER_READY set

**Allocation (5):**
11. Basic allocation succeeds
12. Minimum size enforcement
13. Bump offset advances
14. alloc_count increments
15. Multiple allocations succeed

**Dispose (5):**
16. alloc_count decrements
17. Adds to free list
18. Free block reuse
19. Large allocation
20. Page release on empty

---

## 12. Future Work

**Completed:**
- [x] Implement `slb0_alloc()` with free list + bump allocation
- [x] Implement `slb0_dispose()` with page-level tracking and release

**Pending:**
- [ ] Store allocation size for accurate free list sizing
- [ ] Arena.create/destroy for user scopes (indices 2-15)
- [ ] Frame checkpoint (begin/end) for bulk deallocation
- [ ] sys0_dispose() block coalescing
- [ ] Cross-scope move/copy with SECURE flag enforcement
- [ ] DYNAMIC policy: allocate new pages when chain exhausted
