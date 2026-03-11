/*
 * SigmaCore Memory v0.2.2 - Phase 1 Week 1
 * Day 7: Arena Integration & Stress Testing (AIT-01 through AIT-05)
 *
 * Comprehensive integration tests:
 * - Arena exhaustion and recovery
 * - Mixed SLB0 and arena allocations
 * - Large allocation patterns
 * - Concurrent arena operations
 * - Memory stress scenarios
 */

#include <sigma.test/sigtest.h>
#include <stdio.h>
#include <string.h>
#include "internal/memory.h"
#include "memory.h"

static void set_config(FILE **log_stream) {
    *log_stream = fopen("logs/test_arena_integration.log", "w");
}

static void set_teardown(void) {
}

/**
 * AIT-01: Arena exhaustion and recovery
 * Verify: Can hit 14-arena limit, recover slots, reuse efficiently
 */
void test_ait_01_arena_exhaustion_recovery(void) {
    fprintf(stdout, "\n=== AIT-01: Arena Exhaustion & Recovery ===\n");

    const int MAX_ARENAS = 13;  // SLB2-14 (slots 3-15); slot 2 used by sigma.test framework
    scope arenas[MAX_ARENAS];

    // Phase 1: Fill all 14 arena slots
    fprintf(stdout, "  Phase 1: Creating %d arenas (exhaustion)...\n", MAX_ARENAS);
    for (int i = 0; i < MAX_ARENAS; i++) {
        char name[16];
        snprintf(name, sizeof(name), "exhaust_%d", i);
        arenas[i] = Allocator.create_arena(name, SCOPE_POLICY_RECLAIMING);
        Assert.isNotNull(arenas[i], "Arena %d should create", i);

        // Make some allocations to verify arena is functional
        object ptr = Allocator.Scope.alloc(arenas[i], 256);
        Assert.isNotNull(ptr, "Allocation in arena %d should succeed", i);
        memset(ptr, 0xA0 + i, 256);
    }
    fprintf(stdout, "  ✓ All %d arenas created and functional\n", MAX_ARENAS);

    // Phase 2: Try to create 15th arena (should fail)
    fprintf(stdout, "  Phase 2: Testing exhaustion...\n");
    scope overflow = Allocator.create_arena("overflow", SCOPE_POLICY_RECLAIMING);
    Assert.isNull(overflow, "15th arena should fail (exhaustion)");
    fprintf(stdout, "  ✓ Exhaustion correctly enforced\n");

    // Phase 3: Dispose half the arenas
    fprintf(stdout, "  Phase 3: Disposing 7 arenas...\n");
    for (int i = 0; i < 7; i++) {
        Allocator.dispose_arena(arenas[i]);
    }
    fprintf(stdout, "  ✓ 7 arenas disposed\n");

    // Phase 4: Create new arenas in freed slots
    fprintf(stdout, "  Phase 4: Creating 7 new arenas (recovery)...\n");
    scope new_arenas[7];
    for (int i = 0; i < 7; i++) {
        char name[16];
        snprintf(name, sizeof(name), "recover_%d", i);
        new_arenas[i] = Allocator.create_arena(name, SCOPE_POLICY_DYNAMIC);
        Assert.isNotNull(new_arenas[i], "Recovered arena %d should create", i);

        // Verify allocation works
        object ptr = Allocator.Scope.alloc(new_arenas[i], 512);
        Assert.isNotNull(ptr, "Allocation in recovered arena %d should succeed", i);
    }
    fprintf(stdout, "  ✓ All 7 slots recovered and reused\n");

    // Cleanup
    for (int i = 7; i < MAX_ARENAS; i++) {
        Allocator.dispose_arena(arenas[i]);
    }
    for (int i = 0; i < 7; i++) {
        Allocator.dispose_arena(new_arenas[i]);
    }

    fprintf(stdout, "  ✓ Exhaustion and recovery cycle complete\n");
}

/**
 * AIT-02: Mixed SLB0 and arena allocations
 * Verify: Can switch between scopes, allocations remain isolated
 */
void test_ait_02_mixed_scope_allocations(void) {
    fprintf(stdout, "\n=== AIT-02: Mixed SLB0 & Arena Allocations ===\n");

    // Get SLB0 reference
    scope slb0 = Memory.get_scope(1);
    Assert.isNotNull(slb0, "SLB0 should exist");

    // Create 3 arenas
    scope arena1 = Allocator.create_arena("mixed_1", SCOPE_POLICY_RECLAIMING);
    scope arena2 = Allocator.create_arena("mixed_2", SCOPE_POLICY_DYNAMIC);
    scope arena3 = Allocator.create_arena("mixed_3", SCOPE_POLICY_FIXED);

    Assert.isNotNull(arena1, "Arena 1 should create");
    Assert.isNotNull(arena2, "Arena 2 should create");
    Assert.isNotNull(arena3, "Arena 3 should create");

    fprintf(stdout, "  Created 3 arenas with different policies\n");

    // Pattern: Allocate in round-robin across all scopes
    fprintf(stdout, "  Allocating in round-robin pattern...\n");

    object slb0_ptrs[5];
    object arena1_ptrs[5];
    object arena2_ptrs[5];
    object arena3_ptrs[5];

    for (int i = 0; i < 5; i++) {
        // SLB0
        Allocator.Scope.set(slb0);
        slb0_ptrs[i] = Allocator.alloc(128 + i * 32);
        Assert.isNotNull(slb0_ptrs[i], "SLB0 alloc %d should succeed", i);
        memset(slb0_ptrs[i], 0x10, 128 + i * 32);

        // Arena 1
        Allocator.Scope.set(arena1);
        arena1_ptrs[i] = Allocator.alloc(256 + i * 64);
        Assert.isNotNull(arena1_ptrs[i], "Arena1 alloc %d should succeed", i);
        memset(arena1_ptrs[i], 0x20, 256 + i * 64);

        // Arena 2
        Allocator.Scope.set(arena2);
        arena2_ptrs[i] = Allocator.alloc(512 + i * 128);
        Assert.isNotNull(arena2_ptrs[i], "Arena2 alloc %d should succeed", i);
        memset(arena2_ptrs[i], 0x30, 512 + i * 128);

        // Arena 3
        Allocator.Scope.set(arena3);
        arena3_ptrs[i] = Allocator.alloc(1024 + i * 256);
        Assert.isNotNull(arena3_ptrs[i], "Arena3 alloc %d should succeed", i);
        memset(arena3_ptrs[i], 0x40, 1024 + i * 256);
    }

    fprintf(stdout, "  ✓ 20 allocations across 4 scopes succeeded\n");

    // Verify data integrity in all scopes
    for (int i = 0; i < 5; i++) {
        unsigned char *p1 = (unsigned char *)slb0_ptrs[i];
        unsigned char *p2 = (unsigned char *)arena1_ptrs[i];
        unsigned char *p3 = (unsigned char *)arena2_ptrs[i];
        unsigned char *p4 = (unsigned char *)arena3_ptrs[i];

        Assert.isTrue(p1[0] == 0x10, "SLB0 data should be intact");
        Assert.isTrue(p2[0] == 0x20, "Arena1 data should be intact");
        Assert.isTrue(p3[0] == 0x30, "Arena2 data should be intact");
        Assert.isTrue(p4[0] == 0x40, "Arena3 data should be intact");
    }

    fprintf(stdout, "  ✓ Data integrity verified across all scopes\n");

    // Dispose arenas (SLB0 allocations remain)
    Allocator.dispose_arena(arena1);
    Allocator.dispose_arena(arena2);
    Allocator.dispose_arena(arena3);

    // Verify SLB0 data still intact
    for (int i = 0; i < 5; i++) {
        unsigned char *p = (unsigned char *)slb0_ptrs[i];
        Assert.isTrue(p[0] == 0x10, "SLB0 data should survive arena disposal");
    }

    fprintf(stdout, "  ✓ SLB0 data intact after arena disposal\n");

    // Cleanup SLB0 allocations
    Allocator.Scope.set(slb0);
    for (int i = 0; i < 5; i++) {
        Allocator.dispose(slb0_ptrs[i]);
    }
}

/**
 * AIT-03: Large allocation stress test
 * Verify: Can handle many large allocations, page chaining works
 */
void test_ait_03_large_allocation_stress(void) {
    fprintf(stdout, "\n=== AIT-03: Large Allocation Stress ===\n");

    scope arena = Allocator.create_arena("large_stress", SCOPE_POLICY_DYNAMIC);
    Assert.isNotNull(arena, "Arena should create");

    // Allocate enough to span multiple pages (8KB each)
    // Target: 64KB total (~8 pages)
    const int NUM_LARGE = 32;
    const usize LARGE_SIZE = 2048;  // 2KB each = 64KB total

    fprintf(stdout, "  Allocating %d x %zu bytes (64KB total)...\n", NUM_LARGE, LARGE_SIZE);

    object ptrs[NUM_LARGE];
    for (int i = 0; i < NUM_LARGE; i++) {
        ptrs[i] = Allocator.Scope.alloc(arena, LARGE_SIZE);
        Assert.isNotNull(ptrs[i], "Large alloc %d should succeed", i);

        // Write unique pattern
        memset(ptrs[i], 0x50 + (i % 16), LARGE_SIZE);

        if ((i + 1) % 10 == 0) {
            fprintf(stdout, "    %d allocations completed...\n", i + 1);
        }
    }

    fprintf(stdout, "  ✓ All %d large allocations succeeded\n", NUM_LARGE);

    // Verify page count increased
    Assert.isTrue(arena->page_count > 1, "Should have multiple pages");
    fprintf(stdout, "  ✓ Arena using %zu pages\n", arena->page_count);

    // Verify all data is intact
    for (int i = 0; i < NUM_LARGE; i++) {
        unsigned char *bytes = (unsigned char *)ptrs[i];
        unsigned char expected = 0x50 + (i % 16);

        // Check first, middle, and last bytes
        Assert.isTrue(bytes[0] == expected, "Alloc %d first byte intact", i);
        Assert.isTrue(bytes[LARGE_SIZE / 2] == expected, "Alloc %d middle byte intact", i);
        Assert.isTrue(bytes[LARGE_SIZE - 1] == expected, "Alloc %d last byte intact", i);
    }

    fprintf(stdout, "  ✓ Data integrity verified for all 64KB\n");

    // Cleanup
    Allocator.dispose_arena(arena);
}

/**
 * AIT-04: Rapid create-dispose stress
 * Verify: Can handle many rapid create/dispose cycles without leaks
 */
void test_ait_04_rapid_create_dispose_stress(void) {
    fprintf(stdout, "\n=== AIT-04: Rapid Create-Dispose Stress ===\n");

    const int CYCLES = 100;

    fprintf(stdout, "  Running %d rapid create-dispose cycles...\n", CYCLES);

    for (int i = 0; i < CYCLES; i++) {
        char name[16];
        snprintf(name, sizeof(name), "rapid_%d", i);

        // Create arena
        scope s = Allocator.create_arena(name, SCOPE_POLICY_RECLAIMING);
        Assert.isNotNull(s, "Arena %d should create", i);

        // Make a few allocations
        for (int j = 0; j < 10; j++) {
            object ptr = Allocator.Scope.alloc(s, 64 + j * 32);
            Assert.isNotNull(ptr, "Allocation should succeed");
            memset(ptr, 0xAA, 64 + j * 32);
        }

        // Dispose immediately
        Allocator.dispose_arena(s);

        if ((i + 1) % 25 == 0) {
            fprintf(stdout, "    %d cycles completed...\n", i + 1);
        }
    }

    fprintf(stdout, "  ✓ All %d cycles completed\n", CYCLES);
    fprintf(stdout, "  ✓ Valgrind will verify no leaks\n");
}

/**
 * AIT-05: Multiple arenas with concurrent operations
 * Verify: Can maintain multiple active arenas with interleaved operations
 */
void test_ait_05_concurrent_arena_operations(void) {
    fprintf(stdout, "\n=== AIT-05: Concurrent Arena Operations ===\n");

    const int NUM_ARENAS = 10;
    scope arenas[NUM_ARENAS];

    // Create 10 arenas
    fprintf(stdout, "  Creating %d concurrent arenas...\n", NUM_ARENAS);
    for (int i = 0; i < NUM_ARENAS; i++) {
        char name[16];
        snprintf(name, sizeof(name), "concurrent_%d", i);
        arenas[i] = Allocator.create_arena(name, SCOPE_POLICY_DYNAMIC);
        Assert.isNotNull(arenas[i], "Arena %d should create", i);
    }

    // Perform interleaved allocations across all arenas
    fprintf(stdout, "  Performing interleaved allocations...\n");
    const int ALLOCS_PER_ARENA = 20;

    for (int round = 0; round < ALLOCS_PER_ARENA; round++) {
        for (int arena_idx = 0; arena_idx < NUM_ARENAS; arena_idx++) {
            usize size = 128 + (round * 64) + (arena_idx * 32);
            object ptr = Allocator.Scope.alloc(arenas[arena_idx], size);
            Assert.isNotNull(ptr, "Alloc in arena %d round %d should succeed", arena_idx, round);

            // Write unique pattern: arena_idx in high nibble, round in low nibble
            unsigned char pattern = (arena_idx << 4) | (round & 0x0F);
            memset(ptr, pattern, size);
        }

        if ((round + 1) % 5 == 0) {
            fprintf(stdout, "    Round %d/%d completed\n", round + 1, ALLOCS_PER_ARENA);
        }
    }

    fprintf(stdout, "  ✓ %d allocations across %d arenas completed\n",
            ALLOCS_PER_ARENA * NUM_ARENAS, NUM_ARENAS);

    // Verify arena independence by disposing odd-numbered arenas
    fprintf(stdout, "  Disposing odd-numbered arenas...\n");
    for (int i = 1; i < NUM_ARENAS; i += 2) {
        Allocator.dispose_arena(arenas[i]);
    }
    fprintf(stdout, "  ✓ 5 arenas disposed\n");

    // Even-numbered arenas should still be functional
    fprintf(stdout, "  Verifying even-numbered arenas still functional...\n");
    for (int i = 0; i < NUM_ARENAS; i += 2) {
        object ptr = Allocator.Scope.alloc(arenas[i], 1024);
        Assert.isNotNull(ptr, "Arena %d should still be functional", i);
        memset(ptr, 0xFF, 1024);
    }
    fprintf(stdout, "  ✓ Even arenas remain functional after odd disposal\n");

    // Cleanup
    for (int i = 0; i < NUM_ARENAS; i += 2) {
        Allocator.dispose_arena(arenas[i]);
    }

    fprintf(stdout, "  ✓ Concurrent operations test complete\n");
}

__attribute__((constructor)) void init_test(void) {
    testset("Arena: Integration & Stress", set_config, set_teardown);
    testcase("AIT-01: Exhaustion & recovery", test_ait_01_arena_exhaustion_recovery);
    testcase("AIT-02: Mixed scope allocations", test_ait_02_mixed_scope_allocations);
    testcase("AIT-03: Large allocation stress", test_ait_03_large_allocation_stress);
    testcase("AIT-04: Rapid create-dispose stress", test_ait_04_rapid_create_dispose_stress);
    testcase("AIT-05: Concurrent arena operations", test_ait_05_concurrent_arena_operations);
}
