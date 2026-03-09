# Sigma.Memory v0.2.2 Release Notes

**Release Date:** March 15, 2026  
**Version:** 0.2.2-arenas  
**Status:** Beta Release (Arena System + Dynamic NodePool Growth)  
**Test Coverage:** 31 tests, 100% passing, 0 bytes leaked

---

## Overview

Version 0.2.2 introduces **user arenas** and **dynamic NodePool growth**, providing efficient scope-bound memory management with simple bump allocation. This release completes Phase 1 Week 1 of the v0.2.2 implementation plan.

**Key Innovation:** Arena system replaces the frame-based approach (v0.2.1) with a simpler, faster model that provides O(1) allocation and bulk disposal.

---

## What's New

### 1. User Arena System

**14 Concurrent User Arenas (scope_id 2-15)**
- Create custom memory scopes with `Allocator.create_arena(name, policy)`
- Each arena isolated from others and from SLB0
- Simple bump allocation within 8KB pages
- Bulk disposal with `Allocator.dispose_arena(scope)`

**Arena API:**
```c
// Create arena
scope arena = Allocator.create_arena("WorkBuffer", SCOPE_POLICY_DYNAMIC);

// Allocate from arena
object ptr1 = Scope.alloc_for_scope(arena, 1024);
object ptr2 = Scope.alloc_for_scope(arena, 2048);

// Use arena as current scope
scope prev = Scope.current();
Scope.set(arena);
object ptr3 = Allocator.alloc(512);  // From arena
Scope.set(prev);

// Dispose entire arena (all allocations freed)
Allocator.dispose_arena(arena);
```

**Arena Characteristics:**
- **Allocation speed:** O(1) bump pointer (no B-tree search)
- **Memory overhead:** 0 bytes per allocation (no metadata)
- **Page size:** 8KB with 8128 bytes usable (99.2% efficiency)
- **Growth:** Automatic page chaining via mmap
- **Disposal:** O(P) where P = page count (bulk munmap)

**Use Cases:**
- Request/response processing (HTTP handlers, RPC calls)
- Parsing and compilation passes
- Temporary computation buffers
- Frame-based rendering
- Any scope-bound allocations

### 2. Dynamic NodePool Growth

**Automatic Capacity Expansion via mremap**
- Initial capacity: 8KB (407 page_nodes or 339 btree_nodes)
- Growth sequence: 8KB → 16KB → 32KB → 64KB (doubles each time)
- Growth method: Linux `mremap(MREMAP_MAYMOVE)` with data relocation
- Trigger: When page_node or btree_node allocations collide

**Key Features:**
- **Base relocation support:** Updates scope->nodepool_base after mremap
- **btree_node relocation:** Moves top-down btree region after growth
- **Transparent growth:** Allocations continue seamlessly after expansion
- **Per-scope NodePools:** Each arena has its own growing NodePool

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

### 3. Architectural Improvements

**Scope Structure Enhancements:**
- Added `nodepool_base`: Cached base address per scope
- Added `nodepool_capacity`: Current capacity tracking
- Added `page_alloc_offset`: page_node allocation offset
- Added `btree_alloc_offset`: btree_node allocation offset (from top)
- Added `current_page`: Arena's active page for bump allocation
- Added `current_offset`: Bump pointer within current page
- Added `pagelist_head`: Arena's page chain head

**Memory Efficiency:**
- Arena pages: 64 bytes sentinels (32 header + 32 footer) vs 8192 total = 99.2% usable
- No per-allocation metadata in arenas (vs 24 bytes per allocation in SLB0)
- NodePool overhead: ~0.5% (40-byte header in 8KB+)

---

## Breaking Changes

### Removed: Frame API (v0.2.1)

The frame-based scoped allocation API has been removed and replaced by arenas:

**Removed Functions:**
- `Allocator.frame_begin()` → Use `Allocator.create_arena(name, policy)`
- `Allocator.frame_end(frame)` → Use `Allocator.dispose_arena(scope)`
- `Allocator.frame_depth()` → Not applicable
- `Allocator.frame_allocated(frame)` → Not applicable

**Migration Guide:**
```c
// Old (v0.2.1 - frames)
frame f = Allocator.frame_begin();
object ptr = Allocator.alloc(1024);  // From frame
Allocator.frame_end(f);

// New (v0.2.2 - arenas)
scope arena = Allocator.create_arena("temp", SCOPE_POLICY_DYNAMIC);
object ptr = Scope.alloc_for_scope(arena, 1024);
Allocator.dispose_arena(arena);

// Or as current scope:
scope arena = Allocator.create_arena("temp", SCOPE_POLICY_DYNAMIC);
scope prev = Scope.current();
Scope.set(arena);
object ptr = Allocator.alloc(1024);  // From arena
Scope.set(prev);
Allocator.dispose_arena(arena);
```

**Why the change:**
- Simpler implementation (no chunked allocation complexity)
- Faster allocation (O(1) bump vs O(log n) B-tree for frame chunks)
- Clearer semantics (explicit scope vs implicit frame stack)
- Better resource tracking (pages vs chunks)

---

## Performance Characteristics

| Operation | SYS0 | SLB0 (MTIS) | Arena (NEW) |
|-----------|------|-------------|-------------|
| Allocation | O(n) first-fit | O(log n) B-tree | **O(1) bump** |
| Deallocation | O(n) | O(log n) | N/A (bulk only) |
| Memory overhead | 24 bytes/block | 24 bytes/alloc | **0 bytes** |
| Fragmentation | Reusable blocks | B-tree coalescing | **None** |
| Use case | System metadata | General heap | **Scope-bound** |

**Arena Performance Highlights:**
- ✓ Fastest allocation: Direct pointer arithmetic
- ✓ Zero per-allocation metadata: No btree_node overhead
- ✓ Zero fragmentation: Sequential bump allocation
- ✓ Bulk disposal: Single munmap per page
- ✓ Predictable performance: No B-tree balancing needed

---

## Test Coverage

### Phase 1 Week 1: Complete (31 tests, 0 bytes leaked)

**Day 1-3: NodePool Growth (11 tests)**
- `test/unit/test_nodepool_growth.c` - page_node growth (3 tests)
- `test/unit/test_nodepool_btree_growth.c` - btree_node growth (3 tests)
- `test/validation/test_nodepool_growth_validation.c` - Growth validation (5 tests)

**Day 4: Arena Lifecycle (5 tests)**
- `test/unit/test_arena_lifecycle.c` - Create, initialize, naming, exhaustion

**Day 5: Arena Disposal (5 tests)**
- `test/unit/test_arena_disposal.c` - Unmap pages/NodePool, clear scope, reuse

**Day 6: Arena Allocation (5 tests)**
- `test/unit/test_arena_allocation.c` - Bump allocation, page chaining, large allocs

**Day 7: Integration & Stress (5 tests)**
- `test/integration/test_arena_integration.c` - Exhaustion/recovery, mixed allocations, stress

**Test Execution:**
```bash
# All tests passing
ctest --valgrind
# Result: 31/31 passing, 0 bytes leaked
```

---

## Implementation Details

### NodePool Growth Implementation

**Files Modified:**
- `src/node_pool.c` - Added `grow_nodepool()` function with mremap logic
- `include/internal/node_pool.h` - Growth function declarations
- `src/memory.c` - Integrated NodePool growth triggers

**Growth Trigger Detection:**
```c
// page_node exhaustion
if (page_alloc_offset + sizeof(page_node) >= btree_alloc_offset) {
    grow_nodepool(scope);
}

// btree_node exhaustion
if (btree_alloc_offset - sizeof(btree_node) <= page_alloc_offset) {
    grow_nodepool(scope);
}
```

**Growth Process:**
1. Calculate `new_capacity = old_capacity * 2`
2. Call `mremap(old_base, old_capacity, new_capacity, MREMAP_MAYMOVE)`
3. If base moved, update `scope->nodepool_base`
4. Relocate btree_nodes to new top: `memmove(new_top, old_top, btree_bytes)`
5. Update `btree_alloc_offset` to new top
6. Continue allocation from expanded space

### Arena Implementation

**Files Modified:**
- `src/memory.c` - Added arena_create_impl(), arena_dispose_impl(), arena_alloc()
- `include/memory.h` - Extended sc_allocator_i with create_arena/dispose_arena methods

**Arena Allocation:**
```c
object arena_alloc(scope arena, usize size) {
    // Check if fits in current page
    if (arena->current_offset + size > 8128) {
        // Allocate new page, chain to list
        addr new_page = arena_alloc_new_page(arena);
        arena->current_page = new_page;
        arena->current_offset = 32;  // Skip sentinel
    }
    
    // Bump allocate
    addr result = arena->current_page + arena->current_offset;
    arena->current_offset += size;
    return (object)result;
}
```

**Arena Disposal:**
```c
void arena_dispose_impl(scope arena) {
    // Walk page chain, munmap each
    addr page = arena->pagelist_head;
    while (page != ADDR_EMPTY) {
        addr next = get_next_page(page);
        munmap((void*)page, SYS0_PAGE_SIZE);
        page = next;
    }
    
    // Munmap NodePool
    munmap((void*)arena->nodepool_base, arena->nodepool_capacity);
    
    // Clear scope entry
    memset(arena, 0, sizeof(sc_scope));
    arena->scope_id = 0xFF;  // Mark free
}
```

---

## Known Limitations

1. **Maximum 14 concurrent arenas:** Scope table slots 2-15 (0=SYS0, 1=SLB0)
2. **No individual deallocation:** Arena allocations cannot be freed individually
3. **Single-threaded only:** No thread-safety (waiting for Sigma.Tasking integration)
4. **Memory held until disposal:** Arena pages remain allocated until arena disposed
5. **Page size fixed:** All arena pages are 8KB (SYS0_PAGE_SIZE)

**When NOT to use arenas:**
- Long-lived allocations that need individual deallocation
- Allocations where you can't predict lifecycle boundaries
- Code requiring thread-safe allocation (use SLB0 instead)

---

## Migration Guide

### From v0.2.1 (Frames)

**Replace frame_begin/frame_end:**
```c
// Before (v0.2.1)
frame f = Allocator.frame_begin();
// ... allocations ...
Allocator.frame_end(f);

// After (v0.2.2)
scope arena = Allocator.create_arena("temp", SCOPE_POLICY_DYNAMIC);
scope prev = Scope.current();
Scope.set(arena);
// ... allocations ...
Scope.set(prev);
Allocator.dispose_arena(arena);
```

### From v0.2.0 (No Frames/Arenas)

**Add scoped allocations:**
```c
// Create arena for request processing
scope req_arena = Allocator.create_arena("request", SCOPE_POLICY_DYNAMIC);

// Allocate request-scoped data
object buffer = Scope.alloc_for_scope(req_arena, 4096);
object parsed = Scope.alloc_for_scope(req_arena, sizeof(request_t));

// Process request...

// Free all request data at once
Allocator.dispose_arena(req_arena);
```

---

## Documentation Updates

- ✅ **USERS_GUIDE.md:** Updated to v0.2.2-arenas with comprehensive Arena API documentation
- ✅ **MEMORY_REFERENCE.md:** Updated architecture sections with arena implementation details
- ✅ **ROADMAP.md:** Marked v0.2.2 Phase 1 Week 1 as complete
- ✅ **RELEASE_v0.2.2.md:** This document

---

## What's Next: v0.2.2 Phase 2 (Week 2)

**Focus:** Dog-fooding and refinement
- Integration with Sigma.Test (test framework arena usage)
- Integration with Anvil (command processing arenas)
- Bug fixes based on real-world usage
- Performance profiling and optimization

**Future Phases:**
- Phase 3: Thread-safety hooks (integration points for Sigma.Tasking)
- Phase 4: Advanced arena policies (fixed-size, pre-allocated pools)
- Phase 5: Memory pressure monitoring and statistics

---

## Developer Notes

**Building:**
```bash
cbuild          # Build library
cbuild clean    # Clean build artifacts
```

**Testing:**
```bash
ctest                    # Run all tests
ctest --valgrind         # Run with valgrind leak check
ctest <test_name>        # Run specific test (e.g., ctest arena_lifecycle)
```

**Test Files:**
- Unit tests: `test/unit/test_*.c`
- Validation tests: `test/validation/test_*.c`
- Integration tests: `test/integration/test_*.c`

**Logs:**
- Test logs: `logs/test_*.log`
- Build artifacts: `build/`
- Test executables: `build/test/`

---

## Credits

**Lead Developer:** David Boarman (BadKraft)  
**Project:** QuantumOverride [Q|] - SigmaCore Memory Manager  
**Methodology:** Test-Driven Development (TDD)  
**Test Framework:** Sigma.Test  
**License:** See LICENSE file

---

## Conclusion

v0.2.2-arenas delivers a robust, efficient arena system with dynamic growth capabilities. The simple bump allocation model provides predictable O(1) performance while maintaining clean semantics for scope-bound memory management.

**Key Achievements:**
- ✅ 14 concurrent user arenas
- ✅ O(1) allocation performance
- ✅ Dynamic NodePool growth (8KB→∞)
- ✅ Zero memory leaks (valgrind validated)
- ✅ 31/31 tests passing
- ✅ Comprehensive documentation

The foundation is solid. Time to dog-food! 🐕

---

**Report Issues:** https://github.com/BadKraft/sigma.memory/issues  
**Documentation:** See `docs/` directory  
**Version:** 0.2.2-arenas (March 15, 2026)
