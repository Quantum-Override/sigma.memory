/*
 * Sigma.Memory v0.2.3 - Phase 1 Week 1
 * Day 5: Arena Disposal Testing (ADS-01 through ADS-05)
 *
 * Tests comprehensive arena disposal scenarios:
 * - Empty arenas
 * - Arenas with allocations
 * - Arenas with active frames
 * - Multiple disposal cycles
 */

#include <sigma.test/sigtest.h>
#include <stdio.h>
#include <string.h>
#include "internal/memory.h"
#include "memory.h"

static void set_config(FILE **log_stream) {
    *log_stream = fopen("logs/test_arena_disposal.log", "w");
    // sigma.test framework activates its own arena (slot 2); restore R7 to SLB0.
    Allocator.Scope.restore();
}

static void set_teardown(void) {
}

/**
 * Helper: Count active arenas in scope_table[2-15]
 */
static int count_active_arenas(void) {
    int count = 0;
    for (usize i = 2; i < 16; i++) {
        scope s = Memory.get_scope(i);
        if (s != NULL && s->nodepool_base != ADDR_EMPTY) {
            count++;
        }
    }
    return count;
}

/**
 * ADS-01: Dispose empty arena
 * Verify: Arena disposal works without allocations, slot can be reused
 */
void test_ads_01_dispose_empty_arena(void) {
    fprintf(stdout, "\n=== ADS-01: Dispose Empty Arena ===\n");

    // Create arena
    scope s1 = Allocator.create_arena("empty_test", SCOPE_POLICY_RECLAIMING);
    Assert.isNotNull(s1, "Arena should create successfully");

    uint8_t original_id = s1->scope_id;
    fprintf(stdout, "  Created arena: id=%u, name='%s'\n", original_id, s1->name);

    int active_before = count_active_arenas();
    fprintf(stdout, "  Active arenas before disposal: %d\n", active_before);

    // Dispose immediately (empty arena)
    Allocator.dispose_arena(s1);
    fprintf(stdout, "  Disposed arena\n");

    int active_after = count_active_arenas();
    fprintf(stdout, "  Active arenas after disposal: %d\n", active_after);

    Assert.isTrue(active_after == active_before - 1, "Active count should decrease by 1");

    // Verify slot can be reused
    scope s2 = Allocator.create_arena("reuse_test", SCOPE_POLICY_RECLAIMING);
    Assert.isNotNull(s2, "Should be able to reuse slot");
    Assert.isTrue(s2->scope_id == original_id, "Should reuse same slot ID");

    fprintf(stdout, "  Created new arena in same slot: id=%u\n", s2->scope_id);

    // Cleanup
    Allocator.dispose_arena(s2);
}

/**
 * ADS-02: Dispose multiple arenas simultaneously
 * Verify: Can dispose multiple arenas, slots freed correctly
 */
void test_ads_02_dispose_multiple_arenas(void) {
    fprintf(stdout, "\n=== ADS-02: Dispose Multiple Arenas ===\n");

    const int NUM_ARENAS = 5;
    scope arenas[NUM_ARENAS];

    // Capture baseline: sigma.test arena at slot 2 counts as active
    int baseline = count_active_arenas();

    // Create multiple arenas
    fprintf(stdout, "  Creating %d arenas...\n", NUM_ARENAS);
    for (int i = 0; i < NUM_ARENAS; i++) {
        char name[16];
        snprintf(name, sizeof(name), "multi_%d", i);
        arenas[i] = Allocator.create_arena(name, SCOPE_POLICY_RECLAIMING);
        Assert.isNotNull(arenas[i], "Arena %d should create", i);
    }

    int active_before = count_active_arenas();
    fprintf(stdout, "  Active arenas: %d\n", active_before);
    Assert.isTrue(active_before - baseline == NUM_ARENAS,
                  "Should have %d new active arenas (baseline=%d, got=%d)", NUM_ARENAS, baseline,
                  active_before - baseline);

    // Dispose all arenas
    fprintf(stdout, "  Disposing all %d arenas...\n", NUM_ARENAS);
    for (int i = 0; i < NUM_ARENAS; i++) {
        Allocator.dispose_arena(arenas[i]);
    }

    int active_after = count_active_arenas();
    fprintf(stdout, "  Active arenas after disposal: %d\n", active_after);
    Assert.isTrue(active_after == baseline,
                  "All user arenas should be disposed (expected baseline=%d, got=%d)", baseline,
                  active_after);

    fprintf(stdout, "  ✓ All arenas disposed successfully\n");
}

/**
 * ADS-03: Dispose arena with active frame
 * Verify: Active frame is auto-cleaned on disposal; no crashes; single-frame model enforced
 */
void test_ads_03_dispose_with_active_frames(void) {
    fprintf(stdout, "\n=== ADS-03: Dispose Arena with Active Frame ===\n");

    // Create arena and activate it
    scope s = Allocator.create_arena("frame_test", SCOPE_POLICY_RECLAIMING);
    Assert.isNotNull(s, "Arena should create");
    fprintf(stdout, "  Created arena: id=%lu\n", s->scope_id);

    // Scope.set activates the arena and stores the previous scope in prev chain
    Allocator.Scope.set(s);

    // Open one frame (single-frame-per-scope model, v0.2.3)
    fprintf(stdout, "  Opening frame...\n");
    frame f1 = Allocator.frame_begin();
    Assert.isNotNull(f1, "Frame 1 should create");
    fprintf(stdout, "    Frame 1 created, depth=%zu\n", Allocator.frame_depth());
    Assert.isTrue(Allocator.frame_depth() == 1, "Depth should be 1");

    Allocator.alloc(256);

    // Attempt a second frame while f1 is active — must return NULL
    frame f2 = Allocator.frame_begin();
    Assert.isNull(f2, "Frame 2 should return NULL (single-frame model)");
    fprintf(stdout, "    Duplicate frame_begin() returned NULL as expected\n");
    Assert.isTrue(Allocator.frame_depth() == 1, "Depth should remain 1");

    Allocator.alloc(512);

    fprintf(stdout, "  Frame depth before disposal: %zu\n", Allocator.frame_depth());

    // Restore to SLB0 via the prev-chain (safe — does not corrupt SLB0->prev)
    Allocator.Scope.restore();

    // Dispose arena WITHOUT ending frame (auto-cleanup)
    fprintf(stdout, "  Disposing arena with 1 active frame...\n");
    Allocator.dispose_arena(s);

    fprintf(stdout, "  ✓ Arena disposed without crashes\n");
    Assert.isTrue(Allocator.frame_depth() == 0, "SLB0 frame depth should be 0 after disposal");
}

/**
 * ADS-04: Multiple create-dispose cycles
 * Verify: Can create/dispose same slot multiple times, slot reuse works
 */
void test_ads_04_multiple_disposal_cycles(void) {
    fprintf(stdout, "\n=== ADS-04: Multiple Create-Dispose Cycles ===\n");

    const int CYCLES = 10;
    uint8_t slot_ids[CYCLES];

    for (int i = 0; i < CYCLES; i++) {
        char name[16];
        snprintf(name, sizeof(name), "cycle_%d", i);

        scope s = Allocator.create_arena(name, SCOPE_POLICY_DYNAMIC);
        Assert.isNotNull(s, "Arena %d should create", i);

        slot_ids[i] = s->scope_id;

        // Verify name was set correctly
        Assert.isTrue(strncmp(s->name, name, 15) == 0, "Name should match");

        // Dispose immediately
        Allocator.dispose_arena(s);

        if ((i + 1) % 3 == 0) {
            fprintf(stdout, "  Completed %d cycles\n", i + 1);
        }
    }

    fprintf(stdout, "  All %d cycles completed\n", CYCLES);

    // Verify slot IDs were reused efficiently
    fprintf(stdout, "  Slot IDs used: ");
    for (int i = 0; i < CYCLES; i++) {
        fprintf(stdout, "%d ", slot_ids[i]);
    }
    fprintf(stdout, "\n");

    // Since we dispose before creating next, should reuse the same first slot.
    // Note: slot 2 is occupied by sigma.test framework, so first free slot is 3.
    int first_slot = slot_ids[0];
    int reuse_count = 0;
    for (int i = 0; i < CYCLES; i++) {
        if (slot_ids[i] == first_slot) {
            reuse_count++;
        }
    }

    fprintf(stdout, "  Slot %d reused %d times (efficient reuse)\n", first_slot, reuse_count);
    Assert.isTrue(reuse_count == CYCLES, "Should always reuse same slot %d (got %d/%d)", first_slot,
                  reuse_count, CYCLES);
}

/**
 * ADS-05: Dispose validation (reject invalid scopes)
 * Verify: Cannot dispose SYS0/SLB0, NULL handling
 */
void test_ads_05_dispose_validation(void) {
    fprintf(stdout, "\n=== ADS-05: Dispose Validation ===\n");

    // Test 1: NULL scope (should not crash)
    fprintf(stdout, "  Attempting to dispose NULL scope...\n");
    Allocator.dispose_arena(NULL);
    fprintf(stdout, "  ✓ NULL disposal handled safely\n");

    // Test 2: SYS0 (scope_id=0, should reject)
    fprintf(stdout, "  Attempting to dispose SYS0...\n");
    scope sys0 = Memory.get_scope(0);
    Assert.isNotNull(sys0, "SYS0 should exist");

    addr sys0_base_before = sys0->nodepool_base;
    Allocator.dispose_arena(sys0);
    addr sys0_base_after = sys0->nodepool_base;

    Assert.isTrue(sys0_base_before == sys0_base_after, "SYS0 should not be disposed");
    fprintf(stdout, "  ✓ SYS0 disposal correctly rejected\n");

    // Test 3: SLB0 (scope_id=1, should reject)
    fprintf(stdout, "  Attempting to dispose SLB0...\n");
    scope slb0 = Memory.get_scope(1);
    Assert.isNotNull(slb0, "SLB0 should exist");

    addr slb0_base_before = slb0->nodepool_base;
    Allocator.dispose_arena(slb0);
    addr slb0_base_after = slb0->nodepool_base;

    Assert.isTrue(slb0_base_before == slb0_base_after, "SLB0 should not be disposed");
    fprintf(stdout, "  ✓ SLB0 disposal correctly rejected\n");

    // Test 4: Valid user arena (should succeed)
    fprintf(stdout, "  Creating and disposing valid user arena...\n");
    scope user_arena = Allocator.create_arena("valid", SCOPE_POLICY_RECLAIMING);
    Assert.isNotNull(user_arena, "User arena should create");
    Assert.isTrue(user_arena->scope_id >= 2 && user_arena->scope_id <= 15,
                  "User arena should be in range [2,15]");

    Allocator.dispose_arena(user_arena);
    fprintf(stdout, "  ✓ Valid user arena disposed successfully\n");
}

__attribute__((constructor)) void init_test(void) {
    testset("Arena: Disposal Comprehensive", set_config, set_teardown);
    testcase("ADS-01: Dispose empty arena", test_ads_01_dispose_empty_arena);
    testcase("ADS-02: Dispose multiple arenas", test_ads_02_dispose_multiple_arenas);
    testcase("ADS-03: Dispose with active frames", test_ads_03_dispose_with_active_frames);
    testcase("ADS-04: Multiple create-dispose cycles", test_ads_04_multiple_disposal_cycles);
    testcase("ADS-05: Dispose validation", test_ads_05_dispose_validation);
}
