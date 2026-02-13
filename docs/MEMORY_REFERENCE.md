# SigmaCore Memory System - Architecture & Reference Guide

**Version:** 0.2.0-alpha  
**Date:** February 12, 2026  
**Status:** Alpha Release (Critical bugfix + 2KB NodePool + Dynamic growth)

---

## 1. Overview

The SigmaCore memory management system implements a **B-Tree external metadata architecture** with register-based operation model:

| Component | Description | Strategy |
|-----------|-------------|----------|
| **SYS0** | Bootstrap allocator (8KB static page) | First-fit reclaiming |
| **NodePool** | B-Tree metadata (2KB initial, grows to 32KB+) | External allocation tracking |
| **SLB0** | User-space allocator (16 pages) | 100% page utilization |
| **SLBn** | User-defined arenas (scope_table[2-15]) | Per-slab B-Trees |

### Key Design Decisions (v0.2.0)

- **External Metadata**: B-Tree nodes stored outside user pages (100% page utilization)
- **Register Machine Model**: R1 (NodePool base), R2 (operation results), stack-based params
- **Per-Slab B-Trees**: Each slab maintains its own allocation tree (selective complexity)
- **Log2 Hints**: 8-bit log2 encoding represents 2^0 to 2^255 bytes for first-fit optimization
- **24-byte Nodes**: Cache-friendly metadata (3 nodes per 64-byte cache line)

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

### 2.2 NodePool (2KB Initial, Grows Dynamically)

```
Index       Size    Content                    
────────────────────────────────────────────────────────
0           24      [Reserved - NODE_NULL sentinel]
1-82        24×82   B-Tree nodes (initial capacity)
────────────────────────────────────────────────────────
Total:      2KB (83 nodes × 24 bytes) initial
Growth:     2KB → 4KB → 8KB → 16KB → 32KB (doubles)
```

**NodePool Characteristics:**
- Separate mmap allocation (starts small, grows on demand)
- Base address cached in R1 register
- LIFO free list (uses left_idx as next pointer)
- O(1) indexed access: `base + (idx × 24)`

### 2.3 Layout Constants

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
    // Bits 10-15: reserved for future use
    uint8_t _reserved[6]; //  6: reserved for future extensions
} sc_node;               // 24: Total (3 nodes per 64B cache line)

// Bit masks for info field
#define NODE_SIZE_MASK     0x00FF  // Bits 0-7: log2 size
#define NODE_DIRECTION_BIT 0x0100  // Bit 8: direction  
#define NODE_FREE_FLAG     0x0200  // Bit 9: free flag
#define NODE_RESERVED_MASK 0xFC00  // Bits 10-15: reserved
```

**Node Design Rationale:**
- **24 bytes explicit**: Padded to fit 3 nodes in a 64-byte cache line
- **No in-page metadata**: User pages are 100% payload
- **Log2 hints**: 8 bits represent sizes from 1 byte (2^0) to huge (2^255)
- **Bit-packed flags**: Saves separate flag field, keeps structure lean

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

### 3.4 Scope Entry (64 bytes)

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

### 3.5 Block Header (16 bytes)

```c
typedef struct sc_blk_header {
    uint next_off;   // Offset to next header (0 = last)
    uint size;       // Usable payload size
    sbyte flags;     // BLK_FLAG_FREE | LAST | FOOT
    sbyte _pad[7];   // Alignment
} sc_blk_header;
```

### 3.6 Block Footer (8 bytes)

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

11. Initialize scope_table[1] for SLB0
12. Allocate 16 pages via mmap (64KB)
13. Initialize page sentinels and link chain
14. Set slab_slots[1] = SLB0 page-0 address
15. Set R7 = &scope_table[1] (SLB0 is current)
#endif

~~#ifndef TEST_BOOTSTRAP_ONLY~~
~~11. Initialize scope_table[1] for SLB0~~
~~12. Allocate 16 pages via mmap (64KB)~~
~~13. Initialize page sentinels and link chain~~
~~14. Set slab_slots[1] = SLB0 page-0 address~~
~~15. Set R7 = &scope_table[1] (SLB0 is current)~~
~~#endif~~
*The TEST_BOOTSTRAP_ONLY conditional is no longer present; SLB0/user memory system is always initialized.*
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
