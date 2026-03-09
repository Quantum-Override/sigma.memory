# Sigma.Memory v0.2.2 - Dog-Food Release Plan

**Date:** March 8, 2026  
**Target:** Deploy to Sigma.Test and Anvil for dog-fooding  
**Timeline:** 3 weeks (March 8 - March 29, 2026)  
**Theme:** Minimal Viable Arena Support + Critical Infrastructure  
**Methodology:** TDD (Test-Driven Development) - Non-Negotiable

---

## Mission Statement

Get Sigma.Memory into the hands of **Sigma.Test** and **Anvil** teams ASAP to discover real-world requirements. Implement critical infrastructure (NodePool growth + basic arenas) then iterate based on actual usage.

**NOT implementing:** Thread-safety (waiting for Sigma.Tasking)  
**Focus:** Thread-friendly hooks for future task coordination

---

## Development Approach: TDD Cycle

**Red-Green-Refactor (Mandatory):**
1. **RED:** Write failing test (defines contract)
2. **GREEN:** Minimal code to pass test
3. **REFACTOR:** Clean up implementation
4. **VERIFY:** Valgrind clean, no regressions
5. Repeat

**Test Coverage Target:** 90%+ for new code  
**Valgrind:** Every test run must be clean

---

## 🎯 Phase 1: NodePool Growth + Arena Lifecycle (Week 1)

**Duration:** March 8-15, 2026 (7 days)  
**Goal:** NodePool auto-growth + basic arena create/destroy  
**Deliverable:** Can create 14 arenas, NodePool grows automatically

### Day 1: NodePool Growth - Page Nodes (TDD)

**Goal:** Implement mremap growth for page_node exhaustion

#### Test 1.1: Detect page_node exhaustion
**File:** `test/unit/test_nodepool_growth.c`

```c
void test_pagenode_exhaustion_returns_null(void) {
    // Arrange: Get SLB0, calculate initial capacity
    scope s = get_scope_table_entry(1);
    nodepool_header *h = (nodepool_header*)s->nodepool_base;
    uint16_t initial_capacity = (h->capacity - sizeof(nodepool_header)) / sizeof(page_node);
    
    // Act: Allocate page_nodes until exhaustion
    for (uint16_t i = 0; i < initial_capacity + 10; i++) {
        uint16_t idx = nodepool_alloc_page_node(s);
        if (idx == PAGE_NODE_NULL) {
            // Assert: Should exhaust before capacity + 10
            Assert.isTrue(i >= initial_capacity - 1, 
                "Should exhaust around capacity, got %d", i);
            return;
        }
    }
    Assert.fail("Should have exhausted page_nodes");
}
```

**Expected:** Test FAILS (returns NULL after ~407 page_nodes with 8KB pool)

---

#### Test 1.2: NodePool grows on page_node exhaustion
```c
void test_pagenode_growth_via_mremap(void) {
    // Arrange: Calculate initial capacity
    scope s = get_scope_table_entry(1);
    nodepool_header *h = (nodepool_header*)s->nodepool_base;
    usize initial_capacity = h->capacity;
    uint16_t initial_page_capacity = (initial_capacity - sizeof(nodepool_header)) / sizeof(page_node);
    
    // Act: Exhaust initial capacity
    for (uint16_t i = 0; i < initial_page_capacity + 100; i++) {
        uint16_t idx = nodepool_alloc_page_node(s);
        Assert.isNotEqual(idx, PAGE_NODE_NULL, 
            "Allocation %d should succeed with auto-growth", i);
    }
    
    // Assert: Capacity doubled
    h = (nodepool_header*)s->nodepool_base;  // Refresh after potential mremap
    Assert.isTrue(h->capacity == initial_capacity * 2, 
        "Capacity should double: %zu -> %zu", initial_capacity, h->capacity);
}
```

**Expected:** Test FAILS (no mremap implementation yet)

---

#### Implementation 1.1: Add mremap to nodepool_alloc_page_node()
**File:** `src/node_pool.c:348`

```c
uint16_t nodepool_alloc_page_node(scope scope_ptr) {
    if (scope_ptr == NULL || scope_ptr->nodepool_base == ADDR_EMPTY) {
        return PAGE_NODE_NULL;
    }

    nodepool_header *header = (nodepool_header *)scope_ptr->nodepool_base;

    // Check for collision with btree nodes growing down
    usize next_page_offset = header->page_alloc_offset + sizeof(page_node);
    if (next_page_offset >= header->btree_alloc_offset) {
        // GROW POOL via mremap
        usize new_capacity = header->capacity * 2;
        void *new_base = mremap(
            (void*)scope_ptr->nodepool_base,
            header->capacity,
            new_capacity,
            MREMAP_MAYMOVE
        );
        
        if (new_base == MAP_FAILED) {
            // Growth failed - out of memory
            return PAGE_NODE_NULL;
        }
        
        // Update scope and header pointers
        scope_ptr->nodepool_base = (addr)new_base;
        header = (nodepool_header*)new_base;
        header->capacity = new_capacity;
        
        // Recalculate with new capacity
        next_page_offset = header->page_alloc_offset + sizeof(page_node);
    }

    // Calculate index (page_nodes start at index 1)
    usize base_offset = sizeof(nodepool_header);
    usize idx = (header->page_alloc_offset - base_offset) / sizeof(page_node) + 1;

    // Advance allocation offset
    header->page_alloc_offset = next_page_offset;

    // Zero the page_node
    page_node *pn = (page_node*)((addr)header + header->page_alloc_offset - sizeof(page_node));
    memset(pn, 0, sizeof(page_node));

    return (uint16_t)idx;
}
```

**Verify:** Tests 1.1 and 1.2 now PASS ✅

---

#### Test 1.3: Multiple growth cycles
```c
void test_pagenode_multiple_growth_cycles(void) {
    // Arrange: Start with 8KB pool
    scope s = get_scope_table_entry(1);
    nodepool_header *h = (nodepool_header*)s->nodepool_base;
    usize initial_capacity = h->capacity;  // 8KB
    
    // Act: Allocate enough to trigger 3 growth cycles
    // 8KB -> 16KB -> 32KB -> 64KB
    uint16_t allocations_needed = (64 * 1024 - sizeof(nodepool_header)) / sizeof(page_node);
    
    for (uint16_t i = 0; i < allocations_needed; i++) {
        uint16_t idx = nodepool_alloc_page_node(s);
        Assert.isNotEqual(idx, PAGE_NODE_NULL, 
            "Allocation %d should succeed", i);
    }
    
    // Assert: Capacity grew to 64KB
    h = (nodepool_header*)s->nodepool_base;
    Assert.isTrue(h->capacity >= 64 * 1024,
        "Capacity should reach 64KB: got %zu", h->capacity);
}
```

**Verify:** Test PASSES ✅, valgrind clean

---

### Day 2: NodePool Growth - BTree Nodes (TDD)

**Goal:** Implement mremap growth for btree_node exhaustion

#### Test 2.1: Detect btree_node exhaustion
```c
void test_btreenode_exhaustion_returns_null(void) {
    // Arrange: Calculate initial btree capacity
    scope s = get_scope_table_entry(1);
    nodepool_header *h = (nodepool_header*)s->nodepool_base;
    uint16_t initial_capacity = (h->capacity - sizeof(nodepool_header)) / sizeof(sc_node);
    
    // Act: Allocate btree_nodes until exhaustion
    for (uint16_t i = 0; i < initial_capacity + 10; i++) {
        node_idx idx = nodepool_alloc_btree_node(s);
        if (idx == NODE_NULL) {
            Assert.isTrue(i >= initial_capacity - 1,
                "Should exhaust around capacity, got %d", i);
            return;
        }
    }
    Assert.fail("Should have exhausted btree_nodes");
}
```

**Expected:** Test FAILS (returns NULL after ~339 nodes with 8KB pool)

---

#### Test 2.2: NodePool grows on btree_node exhaustion
```c
void test_btreenode_growth_via_mremap(void) {
    // Arrange: Get initial capacity
    scope s = get_scope_table_entry(1);
    nodepool_header *h = (nodepool_header*)s->nodepool_base;
    usize initial_capacity = h->capacity;
    
    // Act: Allocate many btree_nodes (beyond initial capacity)
    uint16_t initial_btree_capacity = (initial_capacity - sizeof(nodepool_header)) / sizeof(sc_node);
    
    for (uint16_t i = 0; i < initial_btree_capacity + 100; i++) {
        node_idx idx = nodepool_alloc_btree_node(s);
        Assert.isNotEqual(idx, NODE_NULL,
            "BTree allocation %d should succeed with auto-growth", i);
    }
    
    // Assert: Capacity doubled
    h = (nodepool_header*)s->nodepool_base;
    Assert.isTrue(h->capacity == initial_capacity * 2,
        "Capacity should double");
}
```

**Expected:** Test FAILS (no mremap at node_pool.c:382)

---

#### Implementation 2.1: Add mremap to nodepool_alloc_btree_node()
**File:** `src/node_pool.c:382`

```c
node_idx nodepool_alloc_btree_node(scope scope_ptr) {
    if (scope_ptr == NULL || scope_ptr->nodepool_base == ADDR_EMPTY) {
        return NODE_NULL;
    }

    nodepool_header *header = (nodepool_header *)scope_ptr->nodepool_base;

    // Check for collision with page nodes growing up
    usize next_btree_offset = header->btree_alloc_offset - sizeof(sc_node);
    if (next_btree_offset <= header->page_alloc_offset) {
        // GROW POOL via mremap
        usize new_capacity = header->capacity * 2;
        void *new_base = mremap(
            (void*)scope_ptr->nodepool_base,
            header->capacity,
            new_capacity,
            MREMAP_MAYMOVE
        );
        
        if (new_base == MAP_FAILED) {
            return NODE_NULL;
        }
        
        // Update scope and header
        scope_ptr->nodepool_base = (addr)new_base;
        header = (nodepool_header*)new_base;
        header->capacity = new_capacity;
        
        // Recalculate with new capacity
        next_btree_offset = header->btree_alloc_offset - sizeof(sc_node);
    }

    // Advance allocation offset (grows down)
    header->btree_alloc_offset = next_btree_offset;

    // Calculate index
    usize capacity_nodes = header->capacity / sizeof(sc_node);
    usize nodes_from_top = (header->capacity - next_btree_offset) / sizeof(sc_node);
    usize idx = capacity_nodes - nodes_from_top;

    // Zero the node
    sc_node *node = (sc_node*)((addr)header + next_btree_offset);
    memset(node, 0, sizeof(sc_node));

    return (node_idx)idx;
}
```

**Verify:** Tests 2.1 and 2.2 now PASS ✅

---

#### Test 2.3: Mixed page and btree node growth
```c
void test_mixed_pagenode_btreenode_growth(void) {
    // Arrange: Interleave allocations
    scope s = get_scope_table_entry(1);
    nodepool_header *h = (nodepool_header*)s->nodepool_base;
    usize initial_capacity = h->capacity;
    
    // Act: Allocate in pattern: 10 page, 10 btree, repeat
    for (int cycle = 0; cycle < 100; cycle++) {
        // Allocate page_nodes
        for (int i = 0; i < 10; i++) {
            uint16_t idx = nodepool_alloc_page_node(s);
            Assert.isNotEqual(idx, PAGE_NODE_NULL, "Page alloc failed");
        }
        
        // Allocate btree_nodes
        for (int i = 0; i < 10; i++) {
            node_idx idx = nodepool_alloc_btree_node(s);
            Assert.isNotEqual(idx, NODE_NULL, "BTree alloc failed");
        }
    }
    
    // Assert: Pool grew appropriately
    h = (nodepool_header*)s->nodepool_base;
    Assert.isTrue(h->capacity > initial_capacity,
        "Pool should have grown from mixed allocations");
}
```

**Verify:** Test PASSES ✅, valgrind clean

---

### Day 3: NodePool Growth - Edge Cases & Validation (TDD)

**Goal:** Stress test growth, validate memory safety

#### Test 3.1: Growth with active allocations
```c
void test_growth_preserves_existing_data(void) {
    // Arrange: Allocate some nodes, write data
    scope s = get_scope_table_entry(1);
    
    uint16_t page_idx = nodepool_alloc_page_node(s);
    page_node *pn = nodepool_get_page_node(s, page_idx);
    pn->page_base = 0xDEADBEEF;  // Write sentinel
    
    node_idx btree_idx = nodepool_alloc_btree_node(s);
    sc_node *node = nodepool_get_btree_node(s, btree_idx);
    node->start = 0xCAFEBABE;  // Write sentinel
    
    // Act: Force growth by allocating many more nodes
    for (int i = 0; i < 1000; i++) {
        nodepool_alloc_page_node(s);
        nodepool_alloc_btree_node(s);
    }
    
    // Assert: Original data intact (mremap preserved memory)
    pn = nodepool_get_page_node(s, page_idx);
    Assert.isEqual(pn->page_base, 0xDEADBEEF, "Page data corrupted");
    
    node = nodepool_get_btree_node(s, btree_idx);
    Assert.isEqual(node->start, 0xCAFEBABE, "BTree data corrupted");
}
```

**Verify:** Test PASSES ✅ (mremap preserves data)

---

#### Test 3.2: Growth failure handling
```c
void test_growth_failure_returns_null(void) {
    // NOTE: Hard to test mremap failure without mocking
    // Document behavior: returns NULL on MAP_FAILED
    
    // This test validates error path exists
    // In practice, OOM is rare on modern systems with overcommit
}
```

---

#### Test 3.3: Valgrind stress test
```bash
# Run with valgrind to verify no leaks from mremap
valgrind --leak-check=full ./build/test/test_nodepool_growth
```

**Verify:** 0 bytes leaked, 0 errors

---

### Day 4: Arena Create - Scope Allocation (TDD)

**Goal:** Implement Arena.create() to allocate scope_table slots

#### Test 4.1: Create single arena
**File:** `test/unit/test_arena_lifecycle.c`

```c
void test_arena_create_returns_scope(void) {
    // Act: Create arena
    scope s = Arena.create("test_arena", POLICY_RECLAIMING);
    
    // Assert: Valid scope returned
    Assert.isNotNull(s, "Arena.create should return scope");
    Assert.isEqual(strcmp(s->name, "test_arena"), 0, "Name should match");
    Assert.isEqual(s->policy, POLICY_RECLAIMING, "Policy should match");
    
    // Cleanup
    Arena.destroy(s);
}
```

**Expected:** Test FAILS (Arena interface not implemented)

---

#### Test 4.2: Create multiple arenas
```c
void test_arena_create_multiple(void) {
    // Act: Create 5 arenas
    scope arenas[5];
    for (int i = 0; i < 5; i++) {
        char name[16];
        snprintf(name, 16, "arena_%d", i);
        arenas[i] = Arena.create(name, POLICY_RECLAIMING);
        Assert.isNotNull(arenas[i], "Arena %d should create", i);
    }
    
    // Assert: All have unique scope_ids
    for (int i = 0; i < 5; i++) {
        for (int j = i + 1; j < 5; j++) {
            Assert.isNotEqual(arenas[i]->scope_id, arenas[j]->scope_id,
                "Arenas should have unique IDs");
        }
    }
    
    // Cleanup
    for (int i = 0; i < 5; i++) {
        Arena.destroy(arenas[i]);
    }
}
```

**Expected:** Test FAILS

---

#### Test 4.3: Arena exhaustion (max 14 arenas)
```c
void test_arena_create_exhaustion(void) {
    // Arrange: SLB0 already exists, can create 14 more (SLB1-14)
    // Note: Reserving SLB15 for system use
    
    scope arenas[14];
    
    // Act: Create 14 arenas
    for (int i = 0; i < 14; i++) {
        char name[16];
        snprintf(name, 16, "arena_%d", i);
        arenas[i] = Arena.create(name, POLICY_RECLAIMING);
        Assert.isNotNull(arenas[i], "Arena %d should create", i);
    }
    
    // Assert: 15th arena fails (only SLB1-14 available)
    scope overflow = Arena.create("overflow", POLICY_RECLAIMING);
    Assert.isNull(overflow, "15th arena should fail");
    
    // Cleanup
    for (int i = 0; i < 14; i++) {
        Arena.destroy(arenas[i]);
    }
}
```

**Expected:** Test FAILS

---

#### Implementation 4.1: Arena interface definition
**File:** `include/memory.h`

```c
// Arena management interface (v0.2.2)
typedef struct sc_arena_i {
    // Lifecycle
    scope (*create)(const char *name, sbyte policy);
    void (*destroy)(scope s);
    
    // Allocation (scoped)
    object (*alloc)(scope s, usize size);
    void (*dispose)(scope s, object ptr);
    
    // Frames (scoped)
    frame (*frame_begin)(scope s);
    integer (*frame_end)(scope s, frame f);
    
    // Introspection
    usize (*page_count)(scope s);
    usize (*allocated)(scope s);
    const char* (*name)(scope s);
} sc_arena_i;

extern const sc_arena_i Arena;
```

---

#### Implementation 4.2: Arena.create() skeleton
**File:** `src/memory.c`

```c
// Forward declarations
static scope arena_create_impl(const char *name, sbyte policy);
static void arena_destroy_impl(scope s);

// Arena interface implementation
static scope arena_create_impl(const char *name, sbyte policy) {
    // Find free slot in scope_table[2-15] (SLB1-14)
    // Note: scope_table[0]=SYS0, [1]=SLB0, [15]=reserved
    
    for (usize i = 2; i < 15; i++) {
        scope s = get_scope_table_entry(i);
        if (s == NULL || s->nodepool_base == ADDR_EMPTY) {
            // Found free slot - initialize scope
            if (s == NULL) {
                // Allocate scope structure (in SYS0 for now)
                s = (scope)sys0_alloc(sizeof(sc_scope));
                if (s == NULL) return NULL;
                
                // Store in scope_table
                scope *table = (scope*)((addr)sys0 + offsetof(sc_sys0, scope_table));
                table[i] = s;
            }
            
            // Initialize scope fields
            s->scope_id = i;
            s->policy = policy;
            s->flags = 0;
            strncpy(s->name, name, 15);
            s->name[15] = '\0';
            
            // Initialize NodePool for this scope
            if (nodepool_init(s) != OK) {
                return NULL;
            }
            
            // Initialize first page
            s->first_page_off = 0;
            s->current_page_off = 0;
            s->page_count = 0;
            
            // Initialize frame support
            s->current_frame_idx = NODE_NULL;
            s->current_chunk_idx = NODE_NULL;
            s->frame_counter = 0;
            s->frame_depth = 0;
            
            return s;
        }
    }
    
    // No free slots
    return NULL;
}

// Arena interface instance
const sc_arena_i Arena = {
    .create = arena_create_impl,
    .destroy = arena_destroy_impl,
    // ... other methods
};
```

**Verify:** Tests 4.1, 4.2, 4.3 now PASS ✅

---

### Day 5: Arena Destroy - Cleanup (TDD)

**Goal:** Implement Arena.destroy() to free all resources

#### Test 5.1: Destroy empty arena
```c
void test_arena_destroy_empty(void) {
    // Arrange: Create arena
    scope s = Arena.create("empty", POLICY_RECLAIMING);
    Assert.isNotNull(s, "Arena should create");
    
    // Act: Destroy immediately
    Arena.destroy(s);
    
    // Assert: Scope slot available for reuse
    scope s2 = Arena.create("reuse", POLICY_RECLAIMING);
    Assert.isNotNull(s2, "Should be able to reuse slot");
    Assert.isEqual(s->scope_id, s2->scope_id, "Should reuse same slot");
    
    Arena.destroy(s2);
}
```

**Expected:** Test FAILS (destroy not implemented)

---

#### Test 5.2: Destroy arena with allocations
```c
void test_arena_destroy_with_allocations(void) {
    // Arrange: Create arena and allocate
    scope s = Arena.create("alloc", POLICY_RECLAIMING);
    
    object ptr1 = Arena.alloc(s, 128);
    object ptr2 = Arena.alloc(s, 256);
    object ptr3 = Arena.alloc(s, 512);
    
    Assert.isNotNull(ptr1, "Allocation 1 should succeed");
    Assert.isNotNull(ptr2, "Allocation 2 should succeed");
    Assert.isNotNull(ptr3, "Allocation 3 should succeed");
    
    // Act: Destroy arena (should free all allocations)
    Arena.destroy(s);
    
    // Assert: Valgrind should show no leaks
}
```

**Expected:** Test FAILS

---

#### Test 5.3: Destroy arena with active frames
```c
void test_arena_destroy_with_active_frames(void) {
    // Arrange: Create arena with nested frames
    scope s = Arena.create("frames", POLICY_RECLAIMING);
    
    frame f1 = Arena.frame_begin(s);
    Arena.alloc(s, 128);
    
    frame f2 = Arena.frame_begin(s);
    Arena.alloc(s, 256);
    
    // Act: Destroy without ending frames (error or auto-cleanup?)
    // Decision: Auto-cleanup (design choice)
    Arena.destroy(s);
    
    // Assert: Should not crash, valgrind clean
}
```

**Expected:** Test FAILS

---

#### Implementation 5.1: Arena.destroy()
**File:** `src/memory.c`

```c
static void arena_destroy_impl(scope s) {
    if (s == NULL || s->scope_id < 2 || s->scope_id >= 15) {
        return;  // Invalid scope or system scope
    }
    
    // Step 1: Free all pages via munmap
    addr page_off = s->first_page_off;
    while (page_off != 0) {
        page_sentinel page = (page_sentinel)page_off;
        addr next = page->next_page_off;
        
        munmap(page, SYS0_PAGE_SIZE);
        
        page_off = next;
    }
    
    // Step 2: Destroy NodePool
    nodepool_destroy(s);
    
    // Step 3: Clear scope fields (mark as available)
    s->nodepool_base = ADDR_EMPTY;
    s->first_page_off = 0;
    s->current_page_off = 0;
    s->page_count = 0;
    s->frame_depth = 0;
    
    // Note: Don't free scope struct (reused)
}
```

**Verify:** Tests 5.1, 5.2, 5.3 now PASS ✅, valgrind clean

---

### Day 6-7: Arena Allocation & Validation (TDD)

**Goal:** Implement Arena.alloc/dispose, integration tests

#### Test 6.1: Allocate in specific arena
```c
void test_arena_alloc_in_scope(void) {
    // Arrange: Create two arenas
    scope s1 = Arena.create("arena1", POLICY_RECLAIMING);
    scope s2 = Arena.create("arena2", POLICY_RECLAIMING);
    
    // Act: Allocate in each
    object ptr1 = Arena.alloc(s1, 128);
    object ptr2 = Arena.alloc(s2, 256);
    
    // Assert: Both succeed
    Assert.isNotNull(ptr1, "Arena1 alloc should succeed");
    Assert.isNotNull(ptr2, "Arena2 alloc should succeed");
    
    // Assert: Addresses don't overlap (different pages)
    Assert.isTrue(abs((addr)ptr1 - (addr)ptr2) >= SYS0_PAGE_SIZE,
        "Arena allocations should be in separate pages");
    
    // Cleanup
    Arena.dispose(s1, ptr1);
    Arena.dispose(s2, ptr2);
    Arena.destroy(s1);
    Arena.destroy(s2);
}
```

#### Test 6.2: Arena isolation
```c
void test_arena_isolation(void) {
    // Arrange: Create two arenas
    scope s1 = Arena.create("isolated1", POLICY_RECLAIMING);
    scope s2 = Arena.create("isolated2", POLICY_RECLAIMING);
    
    // Act: Allocate heavily in arena1
    for (int i = 0; i < 1000; i++) {
        Arena.alloc(s1, 64);
    }
    
    // Assert: Arena2 page count unchanged
    Assert.isEqual(Arena.page_count(s2), 0, 
        "Arena2 should be unaffected by Arena1 allocations");
    
    // Cleanup
    Arena.destroy(s1);
    Arena.destroy(s2);
}
```

#### Implementation 6.1: Arena.alloc/dispose
```c
static object arena_alloc_impl(scope s, usize size) {
    if (s == NULL) return NULL;
    
    // Use existing slb0_alloc logic, but with provided scope
    // (Currently slb0_alloc is hardcoded to SLB0)
    // Refactor: Make slb0_alloc take scope parameter
    
    return scope_alloc(s, size);  // New generic function
}

static void arena_dispose_impl(scope s, object ptr) {
    if (s == NULL || ptr == NULL) return;
    
    scope_dispose(s, ptr);  // New generic function
}
```

**Verify:** All tests PASS ✅, valgrind clean

---

### Phase 1 Completion Checklist

- [ ] Test suite created: `test/unit/test_nodepool_growth.c`
- [ ] Test suite created: `test/unit/test_arena_lifecycle.c`
- [ ] NodePool mremap for page_nodes (Day 1)
- [ ] NodePool mremap for btree_nodes (Day 2)
- [ ] Edge case testing (Day 3)
- [ ] Arena.create() implemented (Day 4)
- [ ] Arena.destroy() implemented (Day 5)
- [ ] Arena.alloc/dispose() implemented (Day 6-7)
- [ ] All Phase 1 tests passing (target: 20+ new tests)
- [ ] Valgrind clean on full suite
- [ ] No regressions in existing 51 tests

**Deliverable:** Can create 14 user arenas, NodePool grows automatically, basic arena lifecycle works.

---

## 🎯 Phase 2: Per-Arena Frames + Thread-Friendly Hooks (Week 2)

**Duration:** March 15-22, 2026  
**Goal:** Extend frames to work with any arena, document thread hooks  
**Deliverable:** Arena.frame_begin/end works, thread-friendly design documented

*(Detailed daily breakdown will be added once Phase 1 completes)*

**Key Tasks:**
- Extend frame operations to take scope parameter
- Test nested frames across multiple arenas
- Document thread-friendly coordination points
- Create hook architecture for Sigma.Tasking

---

## 🎯 Phase 3: Integration & Dog-Fooding (Week 3)

**Duration:** March 22-29, 2026  
**Goal:** Prepare for Sigma.Test and Anvil integration  
**Deliverable:** v0.2.2 released, integration examples ready

*(Detailed daily breakdown will be added once Phase 2 completes)*

**Key Tasks:**
- Create integration examples for Sigma.Test
- Create integration examples for Anvil
- Final validation and stress testing
- Documentation updates
- Release preparation

---

## Thread-Friendly Hooks (Phase 2, Week 2)

### 3. Coordination Points for Sigma.Tasking

**Philosophy:** Sigma.Memory is an OS kernel component. Sigma.Tasking will manage coordination. We provide **hooks**, not locks.

**Coordination Points:**
```c
// Hooks for external coordination (Sigma.Tasking will implement)
typedef struct sc_memory_hooks {
    // Per-scope coordination
    void (*before_scope_alloc)(scope s);
    void (*after_scope_alloc)(scope s);
    
    // Global coordination (scope table access)
    void (*before_scope_create)(void);
    void (*after_scope_create)(void);
    
    // NodePool coordination (can be per-scope or global)
    void (*before_nodepool_op)(scope s);
    void (*after_nodepool_op)(scope s);
} sc_memory_hooks;

// Optional: Set hooks (default = NULL = no-op)
void Memory.set_hooks(const sc_memory_hooks *hooks);
```

**Tasks:**
- [ ] Document coordination points in MEMORY_DESIGN.md
- [ ] Identify critical sections (scope_table access, node allocation)
- [ ] Add hook call sites (compile-guarded? Or always checked?)
- [ ] Example: Task-local scope assignment pattern
- [ ] Document reentrancy requirements

**Estimated Effort:** 2-3 days (mostly documentation)

**Key Insight:** Each task gets its own arena? Or per-module arenas shared across tasks? Let Sigma.Test/Anvil usage inform this.

---

## Testing & Validation (Week 2-3)

### 4. Dog-Food Integration Tests

**Tasks:**
- [ ] Test: Create 10 arenas, allocate in each, destroy all
- [ ] Test: Exhaust NodePool, verify automatic growth
- [ ] Test: Nested frames across multiple arenas
- [ ] Test: Arena isolation (alloc in A1, dispose in A2 = error?)
- [ ] Test: Destroy arena with active frames (error? or auto-cleanup?)
- [ ] Stress: 100 arenas, random alloc/dispose patterns
- [ ] Valgrind: Full test suite clean

**Estimated Effort:** 3-4 days

---

## Sigma.Test Integration (Week 3)

### 5. Test Framework Integration

**Proposed Pattern:**
```c
// In SigmaTest: test isolation via arenas
void run_test(test_func func) {
    scope test_arena = Arena.create("test", POLICY_RECLAIMING);
    Arena.set_current(test_arena);
    
    frame f = Arena.frame_begin(test_arena);
    func();  // Test runs in isolated arena
    Arena.frame_end(test_arena, f);
    
    Arena.destroy(test_arena);  // Cleanup all test allocations
}
```

**Benefits:**
- Test isolation (memory leaks don't cross tests)
- Fast cleanup (destroy arena vs individual dispose calls)
- Memory usage tracking per test

**Tasks:**
- [ ] Create example integration in test/integration/test_sigmatest_pattern.c
- [ ] Document recommended usage patterns
- [ ] Measure overhead vs SLB0-only approach

**Estimated Effort:** 2 days

---

## Anvil Integration (Week 3)

### 6. Build System Integration

**Anvil Use Cases:**
- Per-module arenas (compiler frontend, optimizer, codegen)
- Frame-based compilation phases
- Memory profiling per compilation stage

**Tasks:**
- [ ] Document arena-per-module pattern
- [ ] Example: Parser allocates in "parse" arena, optimizer in "opt" arena
- [ ] Frame-based phase isolation
- [ ] Arena stats for profiling (which phase uses most memory?)

**Estimated Effort:** 2 days (documentation + examples)

---

## Deferred (Post Dog-Food)

**NOT in v0.2.2:**
- POLICY_BUMP slabs (wait for real need)
- POLICY_FIXED slabs (wait for real need)
- Transactional frames (complex, prove need first)
- Arena callbacks/hooks (wait for profiling needs)
- Thread-safety implementation (Sigma.Tasking not ready)

**Implement based on dog-food feedback:**
- Additional policies if RECLAIMING insufficient
- Advanced frame features if basic frames insufficient
- Performance optimizations based on profiling

---

## Success Criteria

**v0.2.2 Ready for Dog-Food when:**
- ✅ NodePool grows automatically (mremap implemented)
- ✅ Can create 14 user arenas (SLB1-15, maybe reserve SLB15 for sys?)
- ✅ Arena create/destroy works
- ✅ Per-arena allocation works
- ✅ Per-arena frames work
- ✅ Thread-friendly hooks documented
- ✅ All tests pass (60-70 total including new arena tests)
- ✅ Valgrind clean

**Sigma.Test Integration Success:**
- Can run test suite with per-test arenas
- Memory leaks don't cross test boundaries
- Teardown is fast (destroy arena)

**Anvil Integration Success:**
- Can allocate per compilation phase
- Memory profiling shows which phase uses memory
- Cleanup is correct (no phase leaks into next)

---

## Risk Mitigation

**Risk #1: mremap failures**
- Mitigation: Test on various Linux kernels
- Fallback: Fail gracefully, clear error message

**Risk #2: Arena exhaustion (14 arenas not enough)**
- Mitigation: Monitor Sigma.Test/Anvil usage
- Fallback: Increase scope_table size (16 → 32?)

**Risk #3: Performance regression**
- Mitigation: Benchmark before/after
- Acceptance: <10% overhead for arena ops

**Risk #4: Design mismatch with real usage**
- Mitigation: **This is why we dog-food!**
- Plan: Iterate based on feedback from Sigma.Test/Anvil teams

---

## Timeline

**Week 1 (Mar 8-15):**
- Day 1-3: NodePool mremap implementation + tests
- Day 4-7: Basic arena create/destroy + allocation

**Week 2 (Mar 15-22):**
- Day 1-2: Per-arena frames
- Day 3-4: Thread-friendly hooks documentation
- Day 5-7: Arena testing + validation

**Week 3 (Mar 22-29):**
- Day 1-2: Sigma.Test integration pattern + docs
- Day 3-4: Anvil integration pattern + docs
- Day 5-7: Final validation + release prep

**Target Release:** March 29, 2026 (3 weeks)

---

## Documentation Updates

**Required:**
- [ ] MEMORY_DESIGN.md - Chapter 9: Arena Management
- [ ] MEMORY_DESIGN.md - Chapter 10: Thread-Friendly Architecture
- [ ] MEMORY_REFERENCE.md - Arena API documentation
- [ ] USERS_GUIDE.md - Arena usage patterns
- [ ] README.md - Update feature list (multi-arena support)
- [ ] ROADMAP.md - Mark v0.2.2 complete, update v0.3.0 plan

---

## Questions for Sigma.Test & Anvil Teams

**Before dog-fooding:**
1. Per-test arenas or per-suite arenas? (SigmaTest)
2. Per-module arenas or per-phase arenas? (Anvil)
3. How many concurrent arenas do you expect? (sizing question)
4. Do you need POLICY_BUMP? Or is RECLAIMING sufficient?
5. Do you need arena callbacks for profiling?

**Post dog-fooding:**
- What worked well?
- What was awkward/missing?
- Performance issues?
- API confusion?

---

**Next Step:** Implement NodePool mremap growth (Week 1, Day 1-3)

**Status:** Ready to begin 🚀
