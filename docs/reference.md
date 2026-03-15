# Sigma.Memory - Architecture & Reference Guide

**Version:** 0.2.3  
**Date:** March 8, 2026  
**Status:** Beta Release (User Arenas + Dynamic NodePool Growth)

---

## 1. Overview

The SigmaCore memory management system implements a **B-Tree external metadata architecture** with scope-based allocation:

| Component | Description | Strategy |
|-----------|-------------|----------|
| **SYS0** | Bootstrap allocator (8KB static page) | First-fit reclaiming |
| **NodePool** | B-Tree metadata (8KB initial, grows dynamically) | External allocation tracking |
| **SLB0** | Default user allocator (dynamic) | MTIS (Multi-Tier Index Search) |
| **Arenas** | User-defined scopes (slots 2-15) | Simple bump allocation |

### Key Design Decisions (v0.2.2)

- **External Metadata**: B-Tree nodes stored outside user pages (100% page utilization)
- **Dynamic Growth**: NodePool grows via mremap() with capacity doubling
- **Scope Isolation**: Independent allocation domains (SYS0, SLB0, 14 user arenas)
- **Simple Arenas**: Bump allocation within 8KB pages (no MTIS overhead)
- **24-byte Nodes**: Cache-friendly metadata (3 nodes per 64-byte cache line)
- **Arena Simplicity**: No individual deallocation, arena-wide disposal only

---

## 2. Memory Layout

### 2.1 SYS0 Page (8KB Static)

```
Offset      Size    Content                    
────────────────────────────────────────────────────────
RESERVED REGION (0-1535) [Phase 7: Reorganized]
────────────────────────────────────────────────────────
0           64      Registers R0-R7 (8 × 8 bytes)
64          128     SlabTable[16] (16 × 8 bytes)
192         128     Generic Stack[16] (16 × 8 bytes)
320         30      NodeTable[15] (15 × 2 bytes)
350         1186    [Unused reserved space]
────────────────────────────────────────────────────────
DATA AREA (1536-8191)
────────────────────────────────────────────────────────
1536        16      First Block Header         
1552        1024    scope_table[16]            
~2576       24      SlotArray wrapper struct   
...         ...     [Remaining free space]     
8184        8       Footer Marker (0xDEADC0DE) 
────────────────────────────────────────────────────────
Total:      8192 bytes (6656 bytes DAT available)
```

### 2.2 NodePool (8KB Initial, Grows Dynamically)

```
v0.2.2: Dynamic Growth Architecture
────────────────────────────────────────────────────────
Initial:  8KB (407 page_nodes or 339 btree_nodes)
Growth:   8KB → 16KB → 32KB → 64KB → ... (doubles)
Method:   mremap(MREMAP_MAYMOVE) with data relocation
────────────────────────────────────────────────────────

NodePool Layout:
Offset      Size    Content                    
────────────────────────────────────────────────────────
0           40      nodepool_header (capacity, allocators)
40          20×N    page_node array (grows up)
...         ...     [free space]
...         24×M    btree_node array (grows down from top)
────────────────────────────────────────────────────────

Growth Trigger:
- page_nodes: when page_alloc_offset meets btree region
- btree_nodes: when btree_alloc_offset meets page region
- Both: checked on every allocation attempt

Growth Process:
1. Calculate new_capacity = old_capacity * 2
2. mremap(old_base, old_capacity, new_capacity, MREMAP_MAYMOVE)
3. If base moved, update scope->nodepool_base
4. Relocate btree_nodes to new top: memmove(new_top, old_top, btree_bytes)
5. Update btree_alloc_offset to new top
6. Continue allocation from expanded space
```

**NodePool Characteristics:**
- Separate mmap allocation per scope (SYS0, SLB0, each arena)
- Base address cached in scope->nodepool_base
- Top-down btree growth, bottom-up page growth
- Critical: btree data must be relocated after mremap
- O(1) indexed access: `base + (idx × node_size)`

**Growth Example:**
```
Initial 8KB:
[header][page_nodes...][free space][...btree_nodes]
   40        ↑                             ↑
           page_off                    btree_off

After growth to 16KB (base may move):
[header][page_nodes...][larger free space][...btree_nodes]
   40        ↑                                     ↑
           page_off                            btree_off (relocated)
```

### 2.3 User Arena Pages (8KB Each)

```c
// SYS0 Layout (v0.2.0)
#define kAlign 16                    // 16-byte alignment
#define SYS0_PAGE_SIZE 8192          // 8 KB (doubled from v0.1.0)

// Reserved Region
#define SYS0_REGISTERS_OFFSET 0      // R0-R7 (64 bytes)
#define SYS0_REGISTERS_SIZE 64
#define SYS0_SLOTS_OFFSET 64         // Slab slots (128 bytes)
#define SYS0_SLOTS_SIZE 128
#define SYS0_SLOTS_END 192

// B-Tree Support Structures (v0.2.0)
#define SYS0_NODE_TABLE_OFFSET 1320  // NodeTable[15] @ 1320 (30 bytes)
#define SYS0_NODE_TABLE_SIZE 30      // 15 root indices × 2 bytes
#define SYS0_NODE_STACK_OFFSET 1350  // NodeStack @ 1350 (256 bytes) [Phase 7: expanded]
#define SYS0_NODE_STACK_SIZE 256     // 32 slots × 8 bytes (31 usable + 1 depth)

#define SYS0_RESERVED_SIZE 1536      // Extended reserved region

// Data Area
#define FIRST_BLOCK_OFFSET 1536
#define LAST_FOOTER_OFFSET 8184

// Scope Table
#define SCOPE_TABLE_COUNT 16
#define SCOPE_ENTRY_SIZE 64          // 16 × 64 = 1024 bytes

// NodePool (v0.2.0-alpha)
#define NODE_POOL_INITIAL_SIZE (2 * 1024)    // 2KB (start small, grow dynamically)
#define NODE_POOL_INITIAL_COUNT 83           // ~83 nodes initial
#define NODE_SIZE 24                         // sizeof(sc_node)
```

---

## 3. Core Structures

### 3.1 Registers (64 bytes)

```c
typedef struct sc_registers {
    addr R0;  // SYS0 base address (offset resolution)
    addr R1;  // NodePool base address (B-Tree ops)
    addr R2;  // Operation result register (B-Tree ops ONLY)
    addr R3;  // Reserved (available for other subsystems)
    addr R4;  // Reserved (available for other subsystems)
    addr R5;  // Reserved (available for other subsystems)
    addr R6;  // Parent scope pointer (future)
    addr R7;  // Current scope pointer (O(1) scope lookup)
} sc_registers;
```

**Register Roles (CARVED IN STONE):**

| Register | Scope | Purpose | Usage |
|----------|-------|---------|-------|
| **R0** | System-wide | SYS0 base address | All offset calculations |
| **R1** | B-Tree | NodePool base | `get_node(idx)` = R1 + (idx × 24) |
| **R2** | B-Tree ONLY | Operation results | B-Tree ops return via R2 |
| **R3-R5** | Available | Other subsystems | Allocator, scope mgmt, etc. |
| **R6** | Future | Parent scope | Nested scope support |
| **R7** | System-wide | Current scope | Active scope pointer |

**CRITICAL CONVENTION - R2 + Stack for B-Tree Operations:**
```c
// Example: B-Tree search
Stack.push(search_size);    // Push params onto stack
Stack.push(root_idx);       
btree_search();             // Operation executes
// R2 now contains result (node_idx or NODE_NULL)
node_idx result = get_R2();
Stack.clear();              // Clean up stack
```

**R2 is EXCLUSIVELY for B-Tree operations.** Other subsystems use:
- Direct function return values
- R3-R5 registers (available)
- Their own calling conventions

### 3.2 B-Tree Node (24 bytes - Cache Friendly)

```c
typedef uint16_t node_idx;
#define NODE_NULL ((node_idx)0)

typedef struct sc_node {
    addr start;          //  8: allocation start address
    uint32_t length;     //  4: actual size in bytes (up to 4GB)
    uint16_t left_idx;   //  2: left child index (NODE_NULL if none)
    uint16_t right_idx;  //  2: right child index (NODE_NULL if none)
    uint16_t info;       //  2: packed optimization hints and flags
    // Bits 0-7:   log2(max_free_size) - 2^0 to 2^255 bytes
    // Bit 8:      direction (0=left, 1=right subtree has max)
    // Bit 9:      FREE_FLAG (this node is free)
    // Bit 10:     FRAME_FLAG (this node is a frame chunk)
    // Bits 11-15: reserved for future use
    union {
        uint8_t _reserved[6];  //  6: reserved (normal nodes)
        struct {               //  6: frame-specific data (when FRAME_FLAG set)
            usize frame_offset;      // Current bump offset in chunk
            uint16_t next_chunk_idx; // Next 4KB chunk (or NODE_NULL)
            uint16_t frame_id;       // Frame identifier
            uint16_t _pad[3];
        } frame;
    };
} sc_node;               // 24: Total (3 nodes per 64B cache line)

// Bit masks for info field
#define NODE_SIZE_MASK     0x00FF  // Bits 0-7: log2 size
#define NODE_DIRECTION_BIT 0x0100  // Bit 8: direction  
#define NODE_FREE_FLAG     0x0200  // Bit 9: free flag
#define FRAME_NODE_FLAG    0x0400  // Bit 10: frame chunk flag (v0.2.1+)
#define NODE_RESERVED_MASK 0xF800  // Bits 11-15: reserved
```

**Node Design Rationale:**
- **24 bytes explicit**: Padded to fit 3 nodes in a 64-byte cache line
- **No in-page metadata**: User pages are 100% payload
- **Log2 hints**: 8 bits represent sizes from 1 byte (2^0) to huge (2^255)
- **Bit-packed flags**: Saves separate flag field, keeps structure lean
- **Frame union (v0.2.1+)**: Reuses _reserved[6] for frame-specific data when FRAME_NODE_FLAG set

### 3.3 NodeStack (128 bytes in SYS0)

```c
// Stack at SYS0 offset 350
// 8 bytes: depth tracking
// 120 bytes: 15 slots × 8 bytes each

typedef struct sc_stack_i {
    int (*push)(addr value);
    int (*pop)(addr *out_value);
    int (*peek)(addr *out_value);
    bool (*is_empty)(void);
    bool (*is_full)(void);
    usize (*depth)(void);
    void (*clear)(void);
} sc_stack_i;

extern const sc_stack_i Stack;
```

**Stack Usage Pattern:**
```c
// 1. Push operation parameters (LIFO order)
Stack.push(param1);
Stack.push(param2);
Stack.push(param3);

// 2. Call B-Tree operation
btree_operation();  // Operation pops params, computes, stores result in R2

// 3. Read result from R2
node_idx result = /* read R2 */;

// 4. Clear stack
Stack.clear();
```

### 3.4 Scope Entry (200 bytes - Expanded for Frame Support v0.2.1+)

```c
typedef struct sc_frame_state {
    uint16_t frame_id;       // Opaque frame handle
    uint16_t head_chunk_idx; // First chunk node in frame
    usize total_allocated;   // Total bytes allocated in frame
} sc_frame_state;  // 12 bytes

typedef struct sc_scope {
    usize scope_id;             //  8: Index in scope_table (0=SYS0, 1=SLB0, 2-15=user arenas)
    sbyte policy;               //  1: SCOPE_POLICY_* (immutable after creation)
    sbyte flags;                //  1: SCOPE_FLAG_* bitmask (mutable)
    uint16_t current_page_idx;  //  2: DYNAMIC: cached NodePool index of current page (v0.2.4)
    uint32_t slab_bump;         //  4: FIXED: bump offset into slab (0 for all other policies) (v0.2.5)
    addr first_page_off;        //  8: DYNAMIC: base address of first page; FIXED: slab base
    addr current_page_off;      //  8: DYNAMIC: base address of current page; FIXED: slab end sentinel
    usize page_count;           //  8: DYNAMIC: total pages in chain; FIXED: mmap page count
    char name[16];              // 16: Inline scope name (null-terminated, max 15 chars)
    addr nodepool_base;         //  8: Base address of per-scope NodePool mmap (ADDR_EMPTY for FIXED)

    // Frame support (v0.2.3: single active frame per scope)
    uint16_t current_frame_idx; //  2: Head chunk node index (NODE_NULL when no frame active)
    uint16_t current_chunk_idx; //  2: Current bump chunk (may differ from head after chaining)
    uint16_t frame_counter;     //  2: Monotonic frame ID generator (never reset)
    bool frame_active;          //  1: True when a frame is open on this scope
    uint8_t _frame_pad;         //  1: Alignment padding
    scope prev;                 //  8: Previous scope in activation chain (NULL = root)
    sc_frame_state active_frame;//  16: Current frame state (valid only when frame_active)
} sc_scope;                     // Total: 96 bytes

// Frame constants
#define FRAME_CHUNK_SIZE 4096  // 4KB per chunk
#define MAX_FRAME_DEPTH 16     // Maximum frame nesting
```

**Scope Table Layout:**
| Index | Scope | Policy | Flags |
|-------|-------|--------|-------|
| 0 | SYS0 | RECLAIMING | PROTECTED \| PINNED |
| 1 | SLB0 | DYNAMIC | SECURE |
| 2-15 | User arenas (v0.2.2+) | User-defined | User-defined |

**Arena Support Notes (v0.2.2+):**
- Arenas use simple bump allocation within 8KB pages
- Each arena has its own NodePool and PageList
- Maximum 14 concurrent user arenas (slots 2-15)
- Each arena allocated via mmap, cleaned up with munmap
- No MTIS overhead - direct offset-based allocation
- Disposal unmaps all pages and shuts down arena's NodePool

**Frame Support Notes (v0.2.1, deprecated in v0.2.2):**
- Frames were chunked bump allocators (4KB chunks)
- Frame API removed in favor of user arenas
- Arenas provide similar scope isolation with better performance

### 3.5 Arena Architecture (v0.2.2+)

#### 3.5.1 Arena Design Principles

**Why Arenas?**
- **Scope isolation**: Separate user-controlled memory regions
- **Bulk disposal**: Free entire arena at once (O(1) cleanup)
- **Predictable performance**: No search, no fragmentation
- **Simple allocation**: Bump pointer - O(1), no metadata traversal

**Comparison to MTIS (SLB0):**
| Feature | SLB0 (MTIS) | Arena (Bump) |
|---------|-------------|--------------|
| Allocation speed | O(log n) B-tree search | O(1) bump pointer |
| Individual deallocation | ✓ (mark node free) | ✗ (arena-wide only) |
| Fragmentation | Coalescing required | None (sequential) |
| Overhead per allocation | 24 bytes (btree_node) | 0 bytes |
| Use case | General-purpose heap | Scope-bound allocations |

#### 3.5.2 Arena Scope Structure

```c
typedef struct sc_scope_s {
    usize scope_id;                  // 0=SYS0, 1=SLB0, 2-15=arenas
    string name;                     // "SYS0", "SLB0", or user name
    uint flags;                      // SECURE, PROTECTED, PINNED, etc.
    uint policy;                     // RECLAIMING, DYNAMIC, FIXED
    
    // Arena-specific fields (v0.2.2+)
    addr nodepool_base;              // Arena's NodePool base address
    usize nodepool_capacity;         // Current capacity (grows dynamically)
    usize page_alloc_offset;         // page_node allocation offset
    usize btree_alloc_offset;        // btree_node allocation offset (from top)
    
    addr current_page;               // Current page for bump allocation
    usize current_offset;            // Offset within current_page
    addr pagelist_head;              // First page in arena's page chain
    
    // (Other fields: registers, stack, btree_root, etc.)
} sc_scope;
```

**Key Arena Fields:**
- `nodepool_base`: Each arena has own NodePool (starts 8KB, grows dynamically)
- `current_page`: Active page for bump allocations
- `current_offset`: Next allocation offset (0-8160, within page)
- `pagelist_head`: Head of linked page chain (for disposal)

#### 3.5.3 Arena Page Layout (8KB)

```
Arena Page Structure:
Offset      Size    Content                    
────────────────────────────────────────────────────────
0           32      page_sentinel (magic, size, flags, prev/next)
32          8128    usable space for bump allocations
8160        32      footer_sentinel (magic, size, scope_id)
────────────────────────────────────────────────────────
Total:      8192 bytes (8KB)
Usable:     8128 bytes (99.2% efficiency)
```

**Page Sentinel (32 bytes):**
```c
typedef struct page_sentinel {
    uint magic;         // 0xC0DEC0DE
    uint size;          // 8192
    uint flags;         // PAGE_SENTINEL
    uint scope_id;      // 0-15
    addr prev_page;     // Previous page in chain (or 0)
    addr next_page;     // Next page in chain (or 0)
    usize _reserved[2]; // Padding to 32 bytes
} page_sentinel;
```

**Footer Sentinel (32 bytes):**
```c
typedef struct footer_sentinel {
    uint magic;         // 0xDEADC0DE
    uint size;          // 8192
    uint scope_id;      // 0-15
    uint _pad;
    usize _reserved[3]; // Padding to 32 bytes
} footer_sentinel;
```

#### 3.5.4 Arena Allocation Algorithm

```c
object arena_alloc(scope arena, usize size) {
    // 1. Check if size fits in current page
    if (arena->current_offset + size > 8128) {
        // 2. Allocate new page via arena_alloc_new_page()
        addr new_page = arena_alloc_new_page(arena);
        if (new_page == ADDR_EMPTY) return NULL;  // mmap failed
        
        // 3. Link new page to chain
        page_sentinel *sentinel = (page_sentinel*)new_page;
        sentinel->next_page = ADDR_EMPTY;
        sentinel->prev_page = arena->current_page;
        
        if (arena->current_page != ADDR_EMPTY) {
            page_sentinel *old = (page_sentinel*)arena->current_page;
            old->next_page = (addr)new_page;
        } else {
            arena->pagelist_head = (addr)new_page;  // First page
        }
        
        // 4. Update current page
        arena->current_page = (addr)new_page;
        arena->current_offset = 32;  // Skip sentinel
    }
    
    // 5. Bump allocate from current page
    addr allocation = arena->current_page + arena->current_offset;
    arena->current_offset += size;
    
    return (object)allocation;
}
```

**Characteristics:**
- **O(1) allocation**: Direct pointer arithmetic (no search)
- **No metadata per allocation**: No btree_node overhead
- **Page chaining**: When current page exhausted, allocate new page
- **Page size**: Fixed 8KB (SYS0_PAGE_SIZE)

#### 3.5.5 Arena Disposal Algorithm

```c
void arena_dispose_impl(scope arena) {
    // 1. Dispose all pages in chain
    addr current_page = arena->pagelist_head;
    while (current_page != ADDR_EMPTY) {
        page_sentinel *sentinel = (page_sentinel*)current_page;
        addr next = sentinel->next_page;
        
        // Unmap page
        munmap((void*)current_page, SYS0_PAGE_SIZE);
        
        current_page = next;
    }
    
    // 2. Shutdown arena's NodePool
    if (arena->nodepool_base != ADDR_EMPTY) {
        munmap((void*)arena->nodepool_base, arena->nodepool_capacity);
    }
    
    // 3. Clear scope entry in scope_table
    Memory.dispose(arena->name);  // Free name string (in SYS0)
    memset(arena, 0, sizeof(sc_scope));  // Zero scope struct
    arena->scope_id = 0xFF;  // Mark as free
}
```

**Disposal Performance:**
- **O(P) where P = page count**: Simply unmap all pages
- **No individual deallocation**: Bulk disposal only
- **Clean shutdown**: munmap + NodePool cleanup
- **Scope table reuse**: Slot becomes available for new arena

#### 3.5.6 Arena Lifecycle Example

```c
// Create arena
scope user_arena = Allocator.create_arena("WorkBuffer", SCOPE_POLICY_DYNAMIC);
// Internally:
//   - Allocate scope struct in scope_table[2-15]
//   - Allocate initial 8KB NodePool via mmap
//   - Allocate first 8KB page via mmap
//   - Initialize sentinels, set current_offset = 32

// Allocate from arena
object ptr1 = Scope.alloc_for_scope(user_arena, 1024);  // offset 32
object ptr2 = Scope.alloc_for_scope(user_arena, 2048);  // offset 1056
object ptr3 = Scope.alloc_for_scope(user_arena, 8000);  // new page, offset 32

// Dispose entire arena
Allocator.dispose_arena(user_arena);
// Internally:
//   - munmap page 1 (8KB)
//   - munmap page 2 (8KB)
//   - munmap NodePool (8KB)
//   - Clear scope_table entry
//   - Slot 2 now available for reuse
```

#### 3.5.7 Arena Limitations

1. **No individual deallocation**: Cannot free single allocations within arena
2. **Maximum 14 arenas**: Scope table slots 2-15 (0=SYS0, 1=SLB0)
3. **Single-threaded**: No thread-safety guarantees
4. **Page size fixed**: All pages are 8KB (SYS0_PAGE_SIZE)
5. **No coalescing**: Pages remain allocated until arena disposal

**When to Use Arenas:**
- ✓ Request processing (HTTP requests, jobs)
- ✓ Parsing/compilation passes
- ✓ Frame-based rendering
- ✓ Temporary computations
- ✗ Long-lived allocations
- ✗ Frequent individual deallocations

### 3.6 Block Header (16 bytes)

```c
typedef struct sc_blk_header {
    uint next_off;   // Offset to next header (0 = last)
    uint size;       // Usable payload size
    sbyte flags;     // BLK_FLAG_FREE | LAST | FOOT
    sbyte _pad[7];   // Alignment
} sc_blk_header;
```

### 3.7 Block Footer (8 bytes)

```c
typedef struct sc_blk_footer {
    uint magic;      // 0xDEADC0DE
    uint size;       // Duplicate for backward traversal
} sc_blk_footer;
```

---

## 4. B-Tree Operations & Invariants (v0.2.0)

### 4.1 Core Invariants (CARVED IN STONE)

These invariants MUST hold at all times:

**I1. Binary Search Tree Property**
```
For all nodes N:
  - All nodes in left subtree: start < N.start
  - All nodes in right subtree: start > N.start
  - No two nodes have identical start addresses
```

**I2. Free Node Marking**
```
Node is free ⟺ (info & NODE_FREE_FLAG) != 0
Node is allocated ⟺ (info & NODE_FREE_FLAG) == 0
```

**I3. Max-Free Hint Accuracy**
```
For all non-leaf nodes N:
  log2_value = (N.info & NODE_SIZE_MASK)
  direction = (N.info & NODE_DIRECTION_BIT) ? right : left
  
  ⟹ subtree[direction] contains free node with size ≥ 2^log2_value
```

**I4. Node Index Validity**
```
For all node_idx values:
  - NODE_NULL (0) is reserved sentinel
  - Valid indices: 1 ≤ idx < capacity
  - left_idx, right_idx ∈ {NODE_NULL} ∪ [1, capacity)
```

**I5. Length Consistency**
```
For all allocated nodes N:
  - N.length > 0
  - N.length ≤ 4GB (fits in uint32_t)
  - N.start + N.length does not overflow address space
```

**I6. Coalescing Requirement**
```
For all adjacent free nodes F1, F2 where F2.start = F1.start + F1.length:
  ⟹ F1 and F2 MUST be merged (no adjacent free nodes)
```

### 4.2 Foundational Operations

**Op1: btree_search(root_idx, size)**
```
Purpose: Find free node ≥ size using BST + hints
Input:   root_idx (node_idx), size (usize)  
Output:  R2 = node_idx of free node, or NODE_NULL
Time:    O(log n) with hint pruning

Algorithm:
  1. current ← root
  2. best_fit ← NODE_NULL
  3. While current ≠ NODE_NULL:
       node ← get_node(current)
       
       // Is this node free and large enough?
       if (node.info & NODE_FREE_FLAG) and (node.length ≥ size):
           best_fit ← current
           break  // First fit (could continue for best fit)
       
       // Use hint to prune search
       hint_log2 ← node.info & NODE_SIZE_MASK
       hint_size ← 2^hint_log2
       if hint_size < size:
           break  // Subtrees can't satisfy request
       
       // Follow hint direction
       if (node.info & NODE_DIRECTION_BIT):
           current ← node.right_idx
       else:
           current ← node.left_idx
           
  4. R2 ← best_fit
```

**Op2: btree_insert(root_idx, addr, length)**
```
Purpose: Insert new allocation node into tree
Input:   root_idx (node_idx), addr (addr), length (usize)
Output:  R2 = new_root_idx
Time:    O(log n)

Algorithm:
  1. new_idx ← NodePool.alloc_node()
  2. if new_idx == NODE_NULL: ERROR (pool exhausted)
  3. new_node ← get_node(new_idx)
  4. new_node.start ← addr
  5. new_node.length ← length
  6. new_node.left_idx ← NODE_NULL
  7. new_node.right_idx ← NODE_NULL
  8. new_node.info ← 0 (allocated node, no free flag)
  
  9. If root_idx == NODE_NULL:
       R2 ← new_idx  // New root
       return
       
  10. current ← root_idx
  11. While true:
        node ← get_node(current)
        
        if addr < node.start:
            if node.left_idx == NODE_NULL:
                node.left_idx ← new_idx
                break
            current ← node.left_idx
        else if addr > node.start:
            if node.right_idx == NODE_NULL:
                node.right_idx ← new_idx
                break
            current ← node.right_idx
        else:
            ERROR: Duplicate address (Invariant I1 violated)
            
  12. R2 ← root_idx  // Root unchanged
```

**Op3: btree_mark_free(node_idx)**
```
Purpose: Mark allocation as free, update hints up tree
Input:   node_idx (node_idx)
Output:  R2 = OK or ERR
Time:    O(log n)

Algorithm:
  1. node ← get_node(node_idx)
  2. node.info |= NODE_FREE_FLAG  // Set free bit
  3. log2_size ← compute_log2(node.length)
  4. node.info |= (log2_size & NODE_SIZE_MASK)
  
  5. // TODO: Walk up tree updating hints (requires parent pointers or stack)
  6. R2 ← OK
```

**Op4: btree_coalesce(node_idx)**
```
Purpose: Merge adjacent free nodes to prevent fragmentation
Input:   node_idx (node_idx of newly freed node)
Output:  R2 = coalesced_node_idx
Time:    O(log n) per neighbor check

Algorithm:
  1. node ← get_node(node_idx)
  2. end_addr ← node.start + node.length
  
  3. // Find right neighbor (inorder successor with node.start == end_addr)
  4. right_neighbor ← btree_find_by_addr(root, end_addr)
  5. if right_neighbor ≠ NODE_NULL and is_free(right_neighbor):
       // Merge with right
       rn ← get_node(right_neighbor)
       node.length += rn.length
       btree_delete(right_neighbor)  // Remove right from tree
       NodePool.dispose_node(right_neighbor)
       
  6. // Find left neighbor (inorder predecessor with end == node.start)
  7. left_neighbor ← btree_find_predecessor_ending_at(root, node.start)
  8. if left_neighbor ≠ NODE_NULL and is_free(left_neighbor):
       // Merge with left
       ln ← get_node(left_neighbor)
       ln.length += node.length
       btree_delete(node_idx)  // Remove current from tree
       NodePool.dispose_node(node_idx)
       R2 ← left_neighbor
       return
       
  9. R2 ← node_idx  // No coalescing, return original
```

### 4.3 Red-Black Tree Balancing (Phase 7 / v0.1.0)

**Why Balancing Matters:**
- Unbalanced BST degenerates to O(n) with sequential insertions
- Production allocators see sequential patterns frequently
- Red-Black trees guarantee O(log n) worst-case performance

**Red-Black Properties:**
1. Every node is RED or BLACK
2. Root is BLACK
3. All leaves (NULL) are BLACK
4. RED nodes have BLACK children (no consecutive reds)
5. All paths from root to leaves have same number of BLACK nodes

**Maximum Height:** h_max = 2 × log₂(n + 1)

#### 4.3.1 Node Color Encoding

**Steal 1 bit from info field:**
```c
// Bit layout of info field (16-bit):
// [15:9] = unused (7 bits)
// [8]    = NODE_COLOR_BIT (0=BLACK, 1=RED)
// [7]    = NODE_FREE_FLAG  
// [6]    = NODE_DIRECTION_BIT
// [5:0]  = log2 size (0-63, represents 2^0 to 2^63)

#define NODE_COLOR_BIT    0x0100  // Bit 8
#define NODE_COLOR_RED    0x0100
#define NODE_COLOR_BLACK  0x0000

// Check color
bool is_red(btree_node node) {
    return (node->info & NODE_COLOR_BIT) != 0;
}

// Set color
void set_red(btree_node node) {
    node->info |= NODE_COLOR_BIT;
}
void set_black(btree_node node) {
    node->info &= ~NODE_COLOR_BIT;
}
```

**Node structure remains 24 bytes** - no growth!

#### 4.3.2 Stack-Based Parent Tracking

**Problem:** Rotations need parent/grandparent access  
**Traditional Solution:** Add parent pointers (+2 bytes per node)  
**Our Solution:** Use NodeStack to track descent path

**NodeStack Requirements:**
```
Max height for Red-Black tree:
  - 1024 nodes:  2×log₂(1024) = 20 levels
  - 4096 nodes:  2×log₂(4096) = 24 levels
  - 65536 nodes: 2×log₂(65536) = 32 levels

Current: 16 slots (128 bytes) ⚠️ INSUFFICIENT
Phase 7: 32 slots (256 bytes) ✓ SUFFICIENT (supports 4B nodes!)
```

**Insertion Pattern with Stack:**
```c
// Phase 1: Descend and push path
node_idx rb_insert(root, start, length) {
    Stack.clear();
    
    // Descend tree, pushing ancestors
    current = root;
    while (current != NODE_NULL) {
        Stack.push(current);        // Push parent
        
        node = get_node(current);
        if (start < node->start)
            current = node->left_idx;
        else
            current = node->right_idx;
    }
    
    // Insert new red node (default color)
    new_idx = NodePool.alloc_node();
    new_node = get_node(new_idx);
    new_node->start = start;
    new_node->length = length;
    set_red(new_node);  // New nodes are RED
    
    // Link to parent (popped from stack)
    parent_idx = Stack.pop();
    link_child(parent_idx, new_idx, start);
    
    // Phase 2: Fix red-black violations
    return rb_fixup_insert(new_idx);
}

// Phase 2: Fix violations using stack
node_idx rb_fixup_insert(node_idx) {
    while (Stack.depth() >= 2) {  // Need parent + grandparent
        parent_idx = Stack.pop();
        grandparent_idx = Stack.peek();  // Don't pop yet
        
        parent = get_node(parent_idx);
        if (!is_red(parent)) break;  // No violation
        
        grandparent = get_node(grandparent_idx);
        uncle_idx = get_uncle(grandparent, parent_idx);
        
        if (uncle_idx != NODE_NULL && is_red(get_node(uncle_idx))) {
            // Case 1: Recolor
            set_black(parent);
            set_black(get_node(uncle_idx));
            set_red(grandparent);
            node_idx = grandparent_idx;  // Continue up tree
            
        } else {
            // Case 2/3: Rotation needed
            Stack.pop();  // Now pop grandparent
            great_grandparent_idx = Stack.is_empty() ? NODE_NULL : Stack.peek();
            
            new_subtree_root = perform_rotation(
                great_grandparent_idx, 
                grandparent_idx, 
                parent_idx, 
                node_idx
            );
            
            if (great_grandparent_idx != NODE_NULL) {
                update_child_link(great_grandparent_idx, new_subtree_root);
            } else {
                root = new_subtree_root;  // New root
            }
            break;  // Fixed!
        }
    }
    
    // Ensure root is black
    set_black(get_node(root));
    return root;
}
```

#### 4.3.3 Rotation Operations

**Left Rotation:**
```
     |                    |
     x                    y
    / \    left_rot(x)   / \
   a   y   =========>   x   c
      / \              / \
     b   c            a   b
```

**Right Rotation:**
```
     |                    |
     y                    x
    / \   right_rot(y)   / \
   x   c  =========>    a   y
  / \                      / \
 a   b                    b   c
```

**Implementation leverages stack:**
- Parent relationship already known (just popped from stack)
- No need to search for parent or store parent pointers
- Update great-grandparent's child link using stack

#### 4.3.4 Performance Characteristics

| Operation | Unbalanced BST | Red-Black Tree |
|-----------|----------------|----------------|
| Insert | O(1) to O(n) | O(log n) |
| Search | O(1) to O(n) | O(log n) |  
| Delete | O(1) to O(n) | O(log n) |
| Space overhead | 0 bytes | 0 bytes (color bit stolen) |

**Real-World Impact:**
- Sequential allocations: O(n) → O(log n)
- 1024 nodes: worst case 1024 ops → 20 ops
- 4096 nodes: worst case 4096 ops → 24 ops

**No space penalty!** Color bit fits in existing padding.

### 4.4 Edge Cases & Test Strategy

**Edge Case Categories:**

**EC1: Empty Tree**
- Insert into empty tree → becomes root
- Search empty tree → NODE_NULL
- Delete from empty tree → error

**EC2: Single Node**
- Delete only node → empty tree
- Search with exact match → found
- Search with no match → NODE_NULL

**EC3: Degenerate Trees**
- All-left chain (sequential allocations)
- All-right chain (reverse sequential)
- Hint propagation in unbalanced trees

**EC4: Boundary Conditions**
- Size = 1 byte (log2 = 0)
- Size = 4GB (uint32_t max)
- Address wraparound (unlikely but test)
- Node pool exhaustion (all 1023 allocated)

**EC5: Coalescing Scenarios**
- Adjacent free blocks (left + current)
- Adjacent free blocks (current + right)
- Triple merge (left + current + right)
- Non-adjacent free blocks (no merge)

**EC6: Hint Verification**
- Hint says "go left" but best fit is right
- Hint size < request (prune subtree)
- Hint outdated after coalescing

### 4.4 Testing Phases

**Phase 1: Insert + Search (Foundation)**
- Single node insert/search
- Multiple inserts (5-10 nodes)
- BST property verification
- Search hit/miss scenarios

**Phase 2: Free + Coalesce**
- Mark single node free
- Coalesce left neighbor
- Coalesce right neighbor
- Triple coalesce
- Verify no adjacent free nodes (Invariant I6)

**Phase 3: Hint Validation**
- Insert allocations, free some
- Verify hints point to largest free node
- Search using hints (should be faster)
- Update hints after coalescing

**Phase 4: Stress Testing**
- Random allocation pattern (100+ ops)
- Sequential allocations
- Reverse sequential allocations
- Allocation/free interleaved
- Pool exhaustion recovery

**Phase 5: Invariant Checking**
- Automated tree walker verifies I1-I6
- Run after every operation
- Catch corruption early

---

## 5. Allocation Strategies

### 5.1 SYS0 (Reclaiming)

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

### 5.2 SLB0 (MTIS - Metadata-Tracked Indexed Slab)

**Strategy:** B-tree indexed allocations with NodePool for metadata

```
Page 0                    Page 1                    Page N
┌────────────────────┐   ┌────────────────────┐   ┌────────────────────┐
│ Sentinel (32B)     │──▶│ Sentinel (32B)     │──▶│ Sentinel (32B)     │
│ [allocated data]   │   │ [allocated data]   │   │ [allocated data]   │
│ (tracked by btree) │   │ (tracked by btree) │   │ (tracked by btree) │
│ (no free list)     │   │ (no free list)     │   │ (no free list)     │
└────────────────────┘   └────────────────────┘   └────────────────────┘
     8192 bytes               8192 bytes               8192 bytes
```

**Allocation Flow:**
1. Search B-tree for free node ≥ size (O(log n) with hints)
2. If found → mark allocated, return address
3. Else → bump allocate from current page
4. If page exhausted → allocate new page via mmap
5. Insert btree_node into B-tree (O(log n))

**Dispose Flow:**
1. Search B-tree for node matching address
2. Mark node as free (set NODE_FREE_FLAG)
3. Coalesce with adjacent free nodes if possible
4. Update max-free hints up the tree

**Use Cases:**
- General-purpose heap allocations
- Long-lived objects with individual deallocation
- Mixed allocation/deallocation patterns

### 5.3 Arenas (v0.2.2+)

**Strategy:** Simple bump allocation with bulk disposal

```
Page 0                    Page 1                    Page N
┌────────────────────┐   ┌────────────────────┐   ┌────────────────────┐
│ Sentinel (32B)     │──▶│ Sentinel (32B)     │──▶│ Sentinel (32B)     │
│ current_offset: 32 │   │ current_offset: 32 │   │ current_offset: 1280│
│ [allocated data]   │   │ [allocated data]   │   │ [allocated data]   │
│ (no metadata)      │   │ (no metadata)      │   │ (bump pointer)     │
│ Footer (32B)       │   │ Footer (32B)       │   │ Footer (32B)       │
└────────────────────┘   └────────────────────┘   └────────────────────┘
     8192 bytes               8192 bytes               8192 bytes
```

**Allocation Flow:**
1. Check if size fits in current page (current_offset + size ≤ 8128)
2. If yes → allocate at current_offset, bump pointer
3. If no → allocate new page via mmap, chain to pagelist
4. Return pointer (O(1), no metadata)

**Dispose Flow:**
- **Individual disposal**: Not supported
- **Arena disposal**: munmap all pages + NodePool (O(P) where P = page count)

**Use Cases:**
- Request/response processing
- Parsing passes
- Temporary computations
- Scope-bound allocations

---

## 6. Public API

### 6.1 Allocator Interface

```c
// Top-level (uses current scope from R7)
Allocator.alloc(size)              // Allocate from current scope
Allocator.dispose(ptr)             // Dispose to current scope
Allocator.realloc(ptr, size)       // Resize: in-place shrink or alloc+copy+dispose grow

// Explicit scope operations
Allocator.Scope.current()    // Get current scope pointer
Allocator.Scope.set(scope)   // Set current scope (updates R7)
Allocator.Scope.config(scope, mask)  // Query policy/flags
Allocator.Scope.alloc(scope, size)   // Allocate from explicit scope
Allocator.Scope.dispose(scope, ptr)  // Dispose to explicit scope

// Arena operations (v0.2.2+)
Allocator.create_arena(name, policy)         // Create user arena (scope_id 2-15)
Allocator.dispose_arena(scope)               // Dispose entire arena + all pages
// Pure bump arena (v0.2.5 / FT-16)
Allocator.Arena.create_fixed(name, capacity) // Create FIXED arena: contiguous slab, O(1) bump, no NodePool
```

### 6.2 Memory Interface (Internal)

```c
Memory.sys0_size()           // Returns 4096
Memory.state()               // Returns MEM_STATE_* flags
Memory.get_first_header()    // First block header in SYS0
Memory.get_last_footer()     // Footer marker
Memory.get_sys0_base()       // R0 value (SYS0 base address)
Memory.get_slots_base()      // Slab slots array start
Memory.get_slots_end()       // Slab slots array end
Memory.SlabManager->...      // Slab slot operations

// Arena operations (v0.2.2+)
Memory.arena_create_impl(name, policy)   // Internal arena creation
Memory.arena_dispose_impl(scope)         // Internal arena disposal
```

### 6.3 SlabManager Interface (Internal)

```c
SlabManager.init_slab_array()           // Initialize slotarray wrapper
SlabManager.get_slab_slot(index)        // Get page-0 address (uses PArray.get)
SlabManager.set_slab_slot(index, addr)  // Set page-0 address (uses PArray.set)
```

> **Implementation Note:** The slab slot array uses `PArray.get()` and `PArray.set()` 
> for consistency. A `SlotArray.set_at()` method would be a useful addition to sigma.collections.

---

## 7. Memory States

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

## 8. Scope Policies & Flags

### 8.1 Policies (Immutable)

| Policy | Value | Description |
|--------|-------|-------------|
| `SCOPE_POLICY_RECLAIMING` | 0 | SYS0 only; first-fit with block reuse |
| `SCOPE_POLICY_DYNAMIC` | 1 | Auto-grows by chaining pages and NodePool |
| `SCOPE_POLICY_FIXED` | 2 | Pure bump allocator: single contiguous mmap slab, O(1) alloc, NULL when full (no growth, no NodePool) — create via `Arena.create_fixed(name, capacity)` (v0.2.5) |

**Notes:**
- v0.2.2+: DYNAMIC applies to SLB0 and user arenas
- v0.2.5: FIXED uses `slab_bump` offset and `current_page_off` as end sentinel
- NodePool growth: 8KB → 16KB → 32KB (doubles via mremap)
- Page allocation: 8KB per page via mmap

### 8.2 Flags (Mutable)|
| `SCOPE_FLAG_SECURE` | 0x04 | Blocks cross-scope move/copy |

**Notes:**
- v0.2.2+: Frame operations removed, PINNED blocks scope switching only

---

## 9. Initialization Sequence

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

11. Initialize scope_table[1] for SLB0
12. Allocate SLB0 NodePool via mmap (8KB initial)
13. Allocate 16 pages via mmap (128KB total)
14. Initialize page sentinels and link chain
15. Set slab_slots[1] = SLB0 page-0 address
16. Initialize SLB0 B-tree root
17. Set R7 = &scope_table[1] (SLB0 is current)

v0.2.2+: User arenas created on-demand via create_arena()
```

---

## 10. Dispatch Mechanism

Allocation dispatch uses `scope_id` switch instead of function pointers:

```c
object memory_alloc(usize size) {
    scope current = memory_get_current_scope();
    switch (current->scope_id) {
        case 0:  return sys0_alloc(size);     // SYS0 (reclaiming)
        case 1:  return slb0_alloc(size);     // SLB0 (MTIS)
        default: return arena_alloc(current, size);  // User arena (bump)
    }
}
```

**Benefits:**
- Smaller scope struct (~200 bytes vs ~300)
- No function pointer indirection
- Compile-time optimization potential
- Simpler initialization
- Clear dispatch path for arenas (v0.2.2+)

---

## 11. Files Reference

| File | Purpose |
|------|---------|
| `include/memory.h` | Public API (Allocator interface) |
| `include/internal/memory.h` | Internal structures and Memory interface |
| `include/internal/slab_manager.h` | SlabManager interface (deprecated) |
| `include/internal/node_pool.h` | NodePool interface (v0.2.2+) |
| `src/memory.c` | Implementation (allocation, init, arenas) |
| `src/slab_manager.c` | Slab slot array management |
| `src/node_pool.c` | NodePool growth implementation (v0.2.2+) |
| `test/unit/test_bootstrap.c` | SYS0 bootstrap tests (10 tests) |
| `test/unit/test_btree_page.c` | B-tree/page tests (20+ tests) |
| `test/unit/test_arena_*.c` | Arena lifecycle/disposal/allocation (15 tests) |
| `test/validation/test_nodepool_growth_validation.c` | NodePool growth (5 tests) |
| `test/integration/test_arena_integration.c` | Arena integration (5 tests) |

---

## 12. Test Coverage (v0.2.2)

### NodePool Growth Tests (Days 1-3: 11 tests)
**Unit Tests (6):**
1. page_node growth: initial → 8KB growth
2. page_node growth: verify data intact
3. page_node growth: multiple growth cycles
4. btree_node growth: initial → 8KB growth
5. btree_node growth: verify relocation
6. btree_node growth: multiple growth cycles

**Validation Tests (5):**
1. Growth trigger detection (page_node collision)
2. Growth trigger detection (btree_node collision)
3. Data integrity across growth
4. Base address relocation handling
5. Capacity doubling verification

### Arena Lifecycle Tests (Days 4-7: 20 tests)
**Lifecycle (5):**
1. Create arena with valid name/policy
2. Arena initialization (scope_id, NodePool, page)
3. Arena slot allocation (2-15)
4. Arena naming and policy
5. Arena creation failure (exhausted slots)

**Disposal (5):**
1. Dispose arena unmaps pages
2. Dispose arena unmaps NodePool
3. Dispose arena clears scope entry
4. Dispose arena frees slot for reuse
5. Dispose arena with multiple pages

**Allocation (5):

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

### Arena System Tests (v0.2.2: 31 tests)

**NodePool Growth (Days 1-3: 11 tests)**
- Unit: page_node/btree_node growth (6 tests)
- Validation: triggers, integrity, relocation (5 tests)

**Arena Lifecycle (Day 4: 5 tests)**
1. Create arena with valid name/policy
2. Arena initialization (scope_id, NodePool, page)
3. Arena slot allocation (2-15)
4. Arena naming and policy
5. Arena creation failure (exhausted slots)

**Arena Disposal (Day 5: 5 tests)**
1. Dispose arena unmaps pages
2. Dispose arena unmaps NodePool
3. Dispose arena clears scope entry
4. Dispose arena frees slot for reuse
5. Dispose arena with multiple pages

**Arena Allocation (Day 6: 5 tests)**
1. Allocate from arena (bump allocation)
2. Multiple allocations within page
3. Large allocation spanning pages
4. Allocation triggers new page
5. Page chaining verification

**Arena Integration (Day 7: 5 tests)**
1. Arena exhaustion and recovery (14 arena limit)
2. Mixed SLB0/arena allocations
3. Large allocation stress (64KB)
4. Rapid create-dispose cycles (100x)
5. Concurrent operations (10 arenas, 200 allocations)

**Totals:**
- **31 arena system tests** (v0.2.2 Phase 1 Week 1)
- **All tests passing, 0 bytes leaked** (valgrind validated)

---

## 13. Version History

### v0.2.2-arenas (March 2026)
**Arena System + Dynamic NodePool Growth**
- ✅ User arenas: 14 concurrent scopes (slots 2-15)
- ✅ Simple bump allocation: O(1), no metadata overhead
- ✅ NodePool dynamic growth: 8KB→16KB→32KB via mremap
- ✅ Arena API: create_arena(), dispose_arena()
- ✅ Bulk disposal: munmap all pages + NodePool
- ✅ 31 tests across 7 test files
- Removed: Frame API (deprecated in favor of arenas)

### v0.2.1-frames (Prior)
**Frame-based Scoped Allocations**
- Chunked bump allocators (4KB chunks)
- LIFO frame stack (16 levels max)
- Frame API: frame_begin(), frame_end()
- Status: Deprecated, replaced by arenas in v0.2.2

### v0.2.0 (Prior)
**MTIS (Metadata-Tracked Indexed Slab)**
- B-tree indexed allocations
- Max-free hints for O(log n) search
- NodePool for btree_node storage
- SLB0 scope with DYNAMIC policy

### v0.1.0 (Initial)
**SYS0 Bootstrap**
- 4KB system page
- First-fit allocation
- Block headers/footers
- Basic scope infrastructure

---

## 14. Performance Characteristics (v0.2.2)

| Operation | SYS0 | SLB0 (MTIS) | Arena |
|-----------|------|-------------|-------|
| Allocation | O(n) first-fit | O(log n) B-tree | **O(1) bump** |
| Deallocation | O(n) | O(log n) | N/A (bulk only) |
| Memory overhead | 24 bytes/block | 24 bytes/alloc | **0 bytes** |
| Fragmentation | Reusable blocks | B-tree coalescing | **None** |
| Use case | System metadata | General heap | **Scope-bound** |

**Arena Advantages:**
- ✓ Fastest allocation (bump pointer)
- ✓ No per-allocation metadata
- ✓ Zero fragmentation
- ✓ O(1) bulk disposal
- ✓ Predictable performance

**Arena Trade-offs:**
- ✗ No individual deallocation
- ✗ Limited to 14 concurrent arenas
- ✗ Memory held until disposal

---

## 15. Future Work

**Completed in v0.2.2:**
- [x] Arena.create_arena/dispose_arena for user scopes (indices 2-15)
- [x] NodePool dynamic growth via mremap
- [x] Simple bump allocation with page chaining
- [x] Bulk disposal for arenas
- [x] Integration tests for exhaustion/recovery

**Pending:**
- [ ] Thread-safety for arenas (mutex per scope)
- [ ] Arena policy extensions (pre-allocate N pages)
- [ ] Memory pressure signals (warn on high usage)
- [ ] sys0_dispose() block coalescing
- [ ] Cross-scope move/copy with SECURE flag enforcement
- [ ] Red-Black tree balancing for SLB0 B-tree (Phase 7)

---
