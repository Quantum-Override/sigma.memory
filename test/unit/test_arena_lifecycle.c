/*
 * SigmaCore
 * Copyright (c) 2025 David Boarman (BadKraft) and contributors
 * QuantumOverride [Q|]
 * ----------------------------------------------
 * MIT License
 * ----------------------------------------------
 * File: test_arena_lifecycle.c
 * Description: Tests for Arena creation and disposal via Allocator interface
 *              Phase 1 Day 4: Arena.create_arena / Arena.dispose_arena
 */

#include <sigma.test/sigtest.h>
#include "internal/memory.h"
#include "memory.h"
#include <string.h>

#if 1  // Region: Test Set Setup & Teardown
static void set_config(FILE **log_stream) {
    *log_stream = fopen("logs/test_arena_lifecycle.log", "w");
    // sigma.test arena occupies slot 2; restore R7 back to SLB0 so scope
    // operations see the expected baseline (14 user slots minus 1 = 13 free).
    Allocator.Scope.restore();
    sbyte state = Memory.state();
    Assert.isTrue(state & MEM_STATE_READY, "Memory system should be ready");
}

static void set_teardown(void) {
    // Safety net: dispose any arenas leaked by a failed test (slots 3-15).
    // Slot 2 belongs to sigma.test framework — do not touch it.
    for (usize i = 3; i < 16; i++) {
        scope s = Memory.get_scope(i);
        if (s != NULL && s->nodepool_base != ADDR_EMPTY) {
            Allocator.dispose_arena(s);
        }
    }
    // Restore R7 back to SLB0 before next test case.
    Allocator.Scope.restore();
}
#endif

#if 1  // Region: Helper Functions
// Helper to count active arenas (non-NULL, nodepool_base != ADDR_EMPTY)
static int count_active_arenas(void) {
    int count = 0;
    // Check slots 2-15 (SLB1-14, skip SYS0=0 and SLB0=1, reserve 15)
    for (usize i = 2; i < 15; i++) {
        scope s = Memory.get_scope(i);
        if (s != NULL && s->nodepool_base != ADDR_EMPTY) {
            count++;
        }
    }
    return count;
}
#endif

#if 1  // Region: ALC - Arena Lifecycle Tests
// ============================================================================
// ALC-01: Create single arena via Allocator interface
// ============================================================================
void test_allocator_create_arena_returns_scope(void) {
    // Arrange: Count initial arenas
    int initial_arena_count = count_active_arenas();
    printf("  Initial active arenas: %d\n", initial_arena_count);
    
    // Act: Create arena via Allocator interface
    scope s = Allocator.create_arena("test_arena", SCOPE_POLICY_RECLAIMING);
    
    // Assert: Valid scope returned
    Assert.isNotNull(s, "Allocator.create_arena should return valid scope");
    Assert.isTrue(strcmp(s->name, "test_arena") == 0, 
        "Arena name should match: expected 'test_arena', got '%s'", s->name);
    Assert.isTrue(s->policy == SCOPE_POLICY_RECLAIMING,
        "Arena policy should match: expected %d, got %d", 
        SCOPE_POLICY_RECLAIMING, s->policy);
    Assert.isTrue(s->nodepool_base != ADDR_EMPTY,
        "Arena should have NodePool initialized");
    Assert.isTrue(s->scope_id >= 2 && s->scope_id < 15,
        "Arena scope_id should be in range [2,14]: got %zu", s->scope_id);
    
    int final_arena_count = count_active_arenas();
    printf("  Final active arenas: %d\n", final_arena_count);
    Assert.isTrue(final_arena_count == initial_arena_count + 1,
        "Should have created 1 arena");
    
    printf("  Created arena: id=%zu, name='%s', policy=%d\n",
           s->scope_id, s->name, s->policy);
    
    // Cleanup
    Allocator.dispose_arena(s);
}

// ============================================================================
// ALC-02: Create multiple arenas
// ============================================================================
void test_allocator_create_multiple_arenas(void) {
    // Arrange: Count initial arenas
    int initial_arena_count = count_active_arenas();
    printf("  Initial active arenas: %d\n", initial_arena_count);
    
    // Act: Create 5 arenas
    scope arenas[5];
    for (int i = 0; i < 5; i++) {
        char name[16];
        snprintf(name, 16, "arena_%d", i);
        arenas[i] = Allocator.create_arena(name, SCOPE_POLICY_RECLAIMING);
        Assert.isNotNull(arenas[i], "Arena %d should create successfully", i);
        printf("  Created arena %d: id=%zu, name='%s'\n", 
               i, arenas[i]->scope_id, arenas[i]->name);
    }
    
    // Assert: All have unique scope_ids
    for (int i = 0; i < 5; i++) {
        for (int j = i + 1; j < 5; j++) {
            Assert.isTrue(arenas[i]->scope_id != arenas[j]->scope_id,
                "Arenas %d and %d should have different scope_ids: got %zu vs %zu",
                i, j, arenas[i]->scope_id, arenas[j]->scope_id);
        }
    }
    
    int final_arena_count = count_active_arenas();
    printf("  Final active arenas: %d\n", final_arena_count);
    Assert.isTrue(final_arena_count == initial_arena_count + 5,
        "Should have created 5 arenas");
    
    // Cleanup
    for (int i = 0; i < 5; i++) {
        Allocator.dispose_arena(arenas[i]);
    }
}

// ============================================================================
// ALC-03: Arena exhaustion (max 13 user arenas available)
// Note: sigma.test framework pre-allocates sigtest_arena at slot 2, leaving
//       slots 3-15 (13 slots) free for user arenas.
// ============================================================================
void test_allocator_arena_exhaustion(void) {
    // Arrange: SLB0 already exists at index 1; sigma.test uses slot 2.
    //          Available user slots: 3-15 = 13 arenas.
    printf("  Attempting to create 13 arenas (slots 3-15)...\n");
    
    scope arenas[13];
    int created_count = 0;
    
    // Act: Create up to 13 arenas
    for (int i = 0; i < 13; i++) {
        char name[16];
        snprintf(name, 16, "arena_%d", i);
        arenas[i] = Allocator.create_arena(name, SCOPE_POLICY_RECLAIMING);
        
        if (arenas[i] != NULL) {
            created_count++;
            if ((i + 1) % 5 == 0) {
                printf("    Created %d arenas so far...\n", i + 1);
            }
        } else {
            printf("    Failed to create arena %d\n", i);
            break;
        }
    }
    
    printf("  Successfully created %d arenas\n", created_count);
    
    // Assert: Should create all 13 arenas (14 total minus 1 for sigma.test)
    Assert.isTrue(created_count == 13,
        "Should create 13 arenas (slots 3-15): got %d", created_count);
    
    // Assert: 14th arena should fail — slots 2-15 all occupied
    scope overflow = Allocator.create_arena("overflow", SCOPE_POLICY_RECLAIMING);
    Assert.isNull(overflow, 
        "14th arena should fail (scope table exhausted)");
    
    printf("  ✓ Correctly rejected 14th arena (exhaustion)\n");
    
    // Cleanup: Dispose all created arenas
    for (int i = 0; i < created_count; i++) {
        Allocator.dispose_arena(arenas[i]);
    }
}

// ============================================================================
// ALC-04: Arena name truncation (max 15 chars + null)
// ============================================================================
void test_allocator_arena_name_truncation(void) {
    // Arrange: Create arena with long name
    const char *long_name = "this_is_a_very_long_arena_name_that_exceeds_16_chars";
    
    // Act: Create arena
    scope s = Allocator.create_arena(long_name, SCOPE_POLICY_RECLAIMING);
    Assert.isNotNull(s, "Arena should create even with long name");
    
    // Assert: Name truncated to 15 chars (+ null terminator)
    Assert.isTrue(strlen(s->name) <= 15,
        "Arena name should be truncated to 15 chars: got %zu", strlen(s->name));
    Assert.isTrue(strncmp(s->name, long_name, 15) == 0,
        "Arena name should match first 15 chars");
    
    printf("  Original name: '%s'\n", long_name);
    printf("  Truncated name: '%s' (length=%zu)\n", s->name, strlen(s->name));
    
    // Cleanup
    Allocator.dispose_arena(s);
}

// ============================================================================
// ALC-05: Arena disposal clears scope slot
// ============================================================================
void test_allocator_dispose_arena_clears_slot(void) {
    // Arrange: Create arena
    scope s1 = Allocator.create_arena("disposable", SCOPE_POLICY_RECLAIMING);
    Assert.isNotNull(s1, "Arena should create");
    
    usize arena_id = s1->scope_id;
    printf("  Created arena at slot %zu\n", arena_id);
    
    // Act: Dispose arena
    Allocator.dispose_arena(s1);
    printf("  Disposed arena\n");
    
    // Assert: Slot should be cleared (can reuse)
    scope s2 = Allocator.create_arena("reused_slot", SCOPE_POLICY_RECLAIMING);
    Assert.isNotNull(s2, "Should be able to create arena in freed slot");
    
    // May reuse same slot or different slot - both valid
    printf("  Created new arena at slot %zu (original was %zu)\n", 
           s2->scope_id, arena_id);
    
    // Cleanup
    Allocator.dispose_arena(s2);
}
#endif

#if 1  // Region: Test Registration
__attribute__((constructor)) void init_arena_lifecycle_tests(void) 
{
    testset("Arena: Lifecycle via Allocator", set_config, set_teardown);

    testcase("ALC-01: Create single arena", 
             test_allocator_create_arena_returns_scope);
    testcase("ALC-02: Create multiple arenas", 
             test_allocator_create_multiple_arenas);  
    testcase("ALC-03: Arena exhaustion (max 14)", 
             test_allocator_arena_exhaustion);
    testcase("ALC-04: Arena name truncation",
             test_allocator_arena_name_truncation);
    testcase("ALC-05: Dispose arena clears slot",
             test_allocator_dispose_arena_clears_slot);
}
#endif
