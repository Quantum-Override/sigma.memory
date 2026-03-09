/*
 * SigmaCore Memory v0.2.2 - Phase 1 Week 1
 * Day 6: Arena Allocation Testing (AAL-01 through AAL-05)
 * 
 * Tests arena-scoped allocation:
 * - Basic allocation in user arena
 * - Multiple allocations within scope
 * - Disposal of arena allocations
 * - Scope isolation
 * - Frame support in arenas
 */

#include <sigtest/sigtest.h>
#include "memory.h"
#include "internal/memory.h"
#include <stdio.h>
#include <string.h>

static void set_config(FILE **log_stream) {
    *log_stream = fopen("logs/test_arena_allocation.log", "w");
}

static void set_teardown(void) {}

/**
 * AAL-01: Basic allocation in user arena
 * Verify: Can allocate and use memory in arena scope
 */
void test_aal_01_basic_arena_allocation(void) {
    fprintf(stdout, "\n=== AAL-01: Basic Arena Allocation ===\n");
    
    // Create arena
    scope arena = Allocator.create_arena("basic_alloc", SCOPE_POLICY_RECLAIMING);
    Assert.isNotNull(arena, "Arena should create");
    fprintf(stdout, "  Created arena: id=%u\n", arena->scope_id);
    
    // Allocate using Scope.alloc (explicit scope)
    fprintf(stdout, "  Allocating 256 bytes in arena...\n");
    object ptr = Allocator.Scope.alloc(arena, 256);
    Assert.isNotNull(ptr, "Allocation should succeed");
    fprintf(stdout, "  ✓ Allocation succeeded: %p\n", ptr);
    
    // Write data to verify allocation is usable
    memset(ptr, 0xAA, 256);
    fprintf(stdout, "  ✓ Memory is writable\n");
    
    // Verify data
    unsigned char *bytes = (unsigned char *)ptr;
    for (int i = 0; i < 256; i++) {
        Assert.isTrue(bytes[i] == 0xAA, "Byte %d should be 0xAA", i);
    }
    fprintf(stdout, "  ✓ Data integrity verified\n");
    
    // Cleanup
    Allocator.dispose_arena(arena);
    fprintf(stdout, "  ✓ Arena disposed\n");
}

/**
 * AAL-02: Multiple allocations in arena
 * Verify: Can make multiple allocations, all succeed
 */
void test_aal_02_multiple_arena_allocations(void) {
    fprintf(stdout, "\n=== AAL-02: Multiple Arena Allocations ===\n");
    
    scope arena = Allocator.create_arena("multi_alloc", SCOPE_POLICY_RECLAIMING);
    Assert.isNotNull(arena, "Arena should create");
    
    const int NUM_ALLOCS = 10;
    object ptrs[NUM_ALLOCS];
    const usize sizes[10] = {64, 128, 256, 512, 1024, 2048, 128, 256, 512, 1024};
    
    fprintf(stdout, "  Making %d allocations...\n", NUM_ALLOCS);
    for (int i = 0; i < NUM_ALLOCS; i++) {
        ptrs[i] = Allocator.Scope.alloc(arena, sizes[i]);
        Assert.isNotNull(ptrs[i], "Allocation %d (%zu bytes) should succeed", i, sizes[i]);
        
        // Write unique pattern
        memset(ptrs[i], 0x10 + i, sizes[i]);
    }
    fprintf(stdout, "  ✓ All %d allocations succeeded\n", NUM_ALLOCS);
    
    // Verify all allocations have correct data
    for (int i = 0; i < NUM_ALLOCS; i++) {
        unsigned char expected = 0x10 + i;
        unsigned char *bytes = (unsigned char *)ptrs[i];
        for (usize j = 0; j < sizes[i]; j++) {
            Assert.isTrue(bytes[j] == expected, "Alloc %d byte %zu should be 0x%02x", i, j, expected);
        }
    }
    fprintf(stdout, "  ✓ Data integrity verified for all allocations\n");
    
    // Cleanup
    Allocator.dispose_arena(arena);
}

/**
 * AAL-03: Arena allocation with current scope
 * Verify: Can switch to arena and use Allocator.alloc()
 */
void test_aal_03_arena_current_scope_allocation(void) {
    fprintf(stdout, "\n=== AAL-03: Arena as Current Scope ===\n");
    
    scope arena = Allocator.create_arena("current_scope", SCOPE_POLICY_RECLAIMING);
    Assert.isNotNull(arena, "Arena should create");
    
    // Save SLB0 for later
    scope slb0 = Memory.get_scope(1);
    
    // Switch to arena as current scope
    fprintf(stdout, "  Switching to arena as current scope...\n");
    bool set_ok = Allocator.Scope.set(arena);
    Assert.isTrue(set_ok, "Should be able to set arena as current scope");
    
    scope current = (scope)Allocator.Scope.current();
    Assert.isTrue(current == arena, "Current scope should be arena");
    fprintf(stdout, "  ✓ Arena is now current scope\n");
    
    // Allocate using Allocator.alloc (uses current scope)
    fprintf(stdout, "  Allocating using Allocator.alloc()...\n");
    object ptr1 = Allocator.alloc(512);
    object ptr2 = Allocator.alloc(1024);
    
    Assert.isNotNull(ptr1, "Allocation 1 should succeed");
    Assert.isNotNull(ptr2, "Allocation 2 should succeed");
    fprintf(stdout, "  ✓ Allocations succeeded in current arena\n");
    
    // Switch back to SLB0
    Allocator.Scope.set(slb0);
    fprintf(stdout, "  Switched back to SLB0\n");
    
    // Cleanup
    Allocator.dispose_arena(arena);
}

/**
 * AAL-04: Scope isolation
 * Verify: Allocations in different arenas are isolated
 */
void test_aal_04_scope_isolation(void) {
    fprintf(stdout, "\n=== AAL-04: Scope Isolation ===\n");
    
    // Create two arenas
    scope arena1 = Allocator.create_arena("isolated_1", SCOPE_POLICY_RECLAIMING);
    scope arena2 = Allocator.create_arena("isolated_2", SCOPE_POLICY_RECLAIMING);
    
    Assert.isNotNull(arena1, "Arena 1 should create");
    Assert.isNotNull(arena2, "Arena 2 should create");
    Assert.isTrue(arena1->scope_id != arena2->scope_id, "Arenas should have different IDs");
    
    fprintf(stdout, "  Created two arenas: %u and %u\n", arena1->scope_id, arena2->scope_id);
    
    // Allocate in arena 1
    fprintf(stdout, "  Allocating in arena 1...\n");
    object ptr1a = Allocator.Scope.alloc(arena1, 256);
    object ptr1b = Allocator.Scope.alloc(arena1, 512);
    Assert.isNotNull(ptr1a, "Arena 1 alloc A should succeed");
    Assert.isNotNull(ptr1b, "Arena 1 alloc B should succeed");
    memset(ptr1a, 0xAA, 256);
    memset(ptr1b, 0xBB, 512);
    
    // Allocate in arena 2
    fprintf(stdout, "  Allocating in arena 2...\n");
    object ptr2a = Allocator.Scope.alloc(arena2, 256);
    object ptr2b = Allocator.Scope.alloc(arena2, 512);
    Assert.isNotNull(ptr2a, "Arena 2 alloc A should succeed");
    Assert.isNotNull(ptr2b, "Arena 2 alloc B should succeed");
    memset(ptr2a, 0xCC, 256);
    memset(ptr2b, 0xDD, 512);
    
    fprintf(stdout, "  ✓ Allocations in both arenas succeeded\n");
    
    // Verify data in arena 1
    unsigned char *bytes1a = (unsigned char *)ptr1a;
    unsigned char *bytes1b = (unsigned char *)ptr1b;
    for (int i = 0; i < 256; i++) {
        Assert.isTrue(bytes1a[i] == 0xAA, "Arena 1 alloc A data intact");
    }
    for (int i = 0; i < 512; i++) {
        Assert.isTrue(bytes1b[i] == 0xBB, "Arena 1 alloc B data intact");
    }
    
    // Verify data in arena 2
    unsigned char *bytes2a = (unsigned char *)ptr2a;
    unsigned char *bytes2b = (unsigned char *)ptr2b;
    for (int i = 0; i < 256; i++) {
        Assert.isTrue(bytes2a[i] == 0xCC, "Arena 2 alloc A data intact");
    }
    for (int i = 0; i < 512; i++) {
        Assert.isTrue(bytes2b[i] == 0xDD, "Arena 2 alloc B data intact");
    }
    
    fprintf(stdout, "  ✓ Data integrity verified in both arenas (isolated)\n");
    
    // Dispose arena 1 (should not affect arena 2)
    Allocator.dispose_arena(arena1);
    fprintf(stdout, "  Disposed arena 1\n");
    
    // Verify arena 2 data still intact
    for (int i = 0; i < 256; i++) {
        Assert.isTrue(bytes2a[i] == 0xCC, "Arena 2 data should survive arena 1 disposal");
    }
    fprintf(stdout, "  ✓ Arena 2 data intact after arena 1 disposal\n");
    
    // Cleanup
    Allocator.dispose_arena(arena2);
}

/**
 * AAL-05: Different policies
 * Verify: Can create arenas with different policies and allocate
 */
void test_aal_05_different_policies(void) {
    fprintf(stdout, "\n=== AAL-05: Different Allocation Policies ===\n");
    
    // Test RECLAIMING policy
    fprintf(stdout, "  Testing RECLAIMING policy...\n");
    scope reclaim = Allocator.create_arena("reclaim", SCOPE_POLICY_RECLAIMING);
    Assert.isNotNull(reclaim, "RECLAIMING arena should create");
    
    object r_ptr = Allocator.Scope.alloc(reclaim, 512);
    Assert.isNotNull(r_ptr, "RECLAIMING allocation should succeed");
    memset(r_ptr, 0xAA, 512);
    fprintf(stdout, "  ✓ RECLAIMING policy works\n");
    
    // Test DYNAMIC policy
    fprintf(stdout, "  Testing DYNAMIC policy...\n");
    scope dynamic = Allocator.create_arena("dynamic", SCOPE_POLICY_DYNAMIC);
    Assert.isNotNull(dynamic, "DYNAMIC arena should create");
    
    object d_ptr = Allocator.Scope.alloc(dynamic, 512);
    Assert.isNotNull(d_ptr, "DYNAMIC allocation should succeed");
    memset(d_ptr, 0xBB, 512);
    fprintf(stdout, "  ✓ DYNAMIC policy works\n");
    
    // Test FIXED policy
    fprintf(stdout, "  Testing FIXED policy...\n");
    scope fixed = Allocator.create_arena("fixed", SCOPE_POLICY_FIXED);
    Assert.isNotNull(fixed, "FIXED arena should create");
    
    object f_ptr = Allocator.Scope.alloc(fixed, 512);
    Assert.isNotNull(f_ptr, "FIXED allocation should succeed");
    memset(f_ptr, 0xCC, 512);
    fprintf(stdout, "  ✓ FIXED policy works\n");
    
    // Cleanup
    Allocator.dispose_arena(reclaim);
    Allocator.dispose_arena(dynamic);
    Allocator.dispose_arena(fixed);
    
    fprintf(stdout, "  ✓ All policies functional\n");
}

__attribute__((constructor)) void init_test(void) {
    testset("Arena: Allocation Operations", set_config, set_teardown);
    testcase("AAL-01: Basic arena allocation", test_aal_01_basic_arena_allocation);
    testcase("AAL-02: Multiple arena allocations", test_aal_02_multiple_arena_allocations);
    testcase("AAL-03: Arena as current scope", test_aal_03_arena_current_scope_allocation);
    testcase("AAL-04: Scope isolation", test_aal_04_scope_isolation);
    testcase("AAL-05: Different policies", test_aal_05_different_policies);
}
