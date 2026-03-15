/*
 * SigmaCore — Sigma.Memory
 * Copyright (c) 2026 David Boarman (BadKraft) and contributors
 * QuantumOverride [Q|]
 * ----------------------------------------------
 * File: test/unit/test_fixed_arena.c
 * Description: Unit tests for SCOPE_POLICY_FIXED pure bump arena (FT-16)
 *              FA-01..FA-09
 */

#include <sigma.test/sigtest.h>
#include "internal/memory.h"
#include "memory.h"

/* ── helpers ──────────────────────────────────────────────────────────────── */

static void set_config(FILE **log_stream) {
    *log_stream = fopen("logs/test_fixed_arena.log", "w");
    Allocator.Scope.restore();
}

static void set_teardown(void) {
    // Safety net: dispose any FIXED arenas leaked by a failing test (slots 3–15).
    // Slot 2 belongs to sigma.test framework — do not touch it.
    for (usize i = 3; i < 16; i++) {
        scope s = Memory.get_scope(i);
        if (s == NULL || s->scope_id == 0) continue;
        Allocator.Arena.dispose(s);
    }
    // Restore R7 back to SLB0 before next test case.
    Allocator.Scope.restore();
}

/* ══════════════════════════════════════════════════════════════════════════ */
#if 1  // Region: FA-01 — create + basic alloc

/**
 * FA-01: Arena.create_fixed returns a valid scope with the right policy,
 *        Scope.alloc on it returns a non-NULL pointer, and R7 advances.
 */
void test_fa_01_create_and_alloc(void) {
    scope fixed = Allocator.Arena.create_fixed("fa01", 4096);
    Assert.isNotNull(fixed, "FA-01: create_fixed must return non-NULL");
    Assert.isTrue(fixed->policy == SCOPE_POLICY_FIXED, "FA-01: policy must be SCOPE_POLICY_FIXED");
    Assert.isTrue(Allocator.Scope.current() == fixed,
                  "FA-01: R7 must point to fixed arena after create");

    object ptr = Allocator.Scope.alloc(fixed, 64);
    Assert.isNotNull(ptr, "FA-01: alloc from fixed arena must succeed");
    Allocator.Arena.dispose(fixed);
}

#endif
/* ══════════════════════════════════════════════════════════════════════════ */
#if 1  // Region: FA-02 — capacity enforcement

/**
 * FA-02: Allocating past the slab capacity returns NULL (no growth).
 */
void test_fa_02_capacity_enforced(void) {
    // 1-page slab (8192 bytes); alloc 8192-byte block must fail (0 headroom)
    scope fixed = Allocator.Arena.create_fixed("fa02", 4096);
    Assert.isNotNull(fixed, "FA-02: create_fixed must succeed");

    // Drain the slab in 256-byte increments (4096 / 256 = 16 allocs)
    for (int i = 0; i < 16; i++) {
        object p = Allocator.Scope.alloc(fixed, 256);
        Assert.isNotNull(p, "FA-02: alloc %d should succeed", i);
    }

    // Next alloc must return NULL (full)
    object over = Allocator.Scope.alloc(fixed, 16);
    Assert.isNull(over, "FA-02: alloc past capacity must return NULL");
    Allocator.Arena.dispose(fixed);
}

#endif
/* ══════════════════════════════════════════════════════════════════════════ */
#if 1  // Region: FA-03 — bump contiguity

/**
 * FA-03: Sequential allocs are strictly adjacent (pure bump semantics).
 */
void test_fa_03_bump_contiguity(void) {
    scope fixed = Allocator.Arena.create_fixed("fa03", 4096);
    Assert.isNotNull(fixed, "FA-03: create_fixed must succeed");

    object a = Allocator.Scope.alloc(fixed, 64);
    object b = Allocator.Scope.alloc(fixed, 64);
    Assert.isNotNull(a, "FA-03: first alloc must succeed");
    Assert.isNotNull(b, "FA-03: second alloc must succeed");

    // b must immediately follow a
    Assert.isTrue((uintptr_t)b == (uintptr_t)a + 64,
                  "FA-03: bump pointer must advance by exactly alloc size");
    Allocator.Arena.dispose(fixed);
}

#endif
/* ══════════════════════════════════════════════════════════════════════════ */
#if 1  // Region: FA-04 — 16-byte alignment

/**
 * FA-04: All allocations are 16-byte aligned regardless of requested size.
 */
void test_fa_04_alignment(void) {
    scope fixed = Allocator.Arena.create_fixed("fa04", 4096);
    Assert.isNotNull(fixed, "FA-04: create_fixed must succeed");

    for (usize sz = 1; sz <= 128; sz++) {
        object p = Allocator.Scope.alloc(fixed, sz);
        // Stop if slab full (test doesn't need every size to fit)
        if (p == NULL) break;
        Assert.isTrue(((uintptr_t)p & 0xF) == 0, "FA-04: alloc(size=%zu) must be 16-byte aligned",
                      sz);
    }
    Allocator.Arena.dispose(fixed);
}

#endif
/* ══════════════════════════════════════════════════════════════════════════ */
#if 1  // Region: FA-05 — no NodePool

/**
 * FA-05: Fixed arenas must not initialise a NodePool (nodepool_base == ADDR_EMPTY).
 */
void test_fa_05_no_nodepool(void) {
    scope fixed = Allocator.Arena.create_fixed("fa05", 4096);
    Assert.isNotNull(fixed, "FA-05: create_fixed must succeed");
    Assert.isTrue(fixed->nodepool_base == ADDR_EMPTY,
                  "FA-05: nodepool_base must be ADDR_EMPTY (no MTIS for FIXED)");
    Allocator.Arena.dispose(fixed);
}

#endif
/* ══════════════════════════════════════════════════════════════════════════ */
#if 1  // Region: FA-06 — multi-page slab

/**
 * FA-06: create_fixed with capacity > 8 KB allocates across multiple pages.
 *        Verify page_count and that allocations span the full capacity.
 */
void test_fa_06_multi_page(void) {
    const usize CAPACITY = 3 * SYS0_PAGE_SIZE;  // 24 KB = 3 pages
    scope fixed = Allocator.Arena.create_fixed("fa06", CAPACITY);
    Assert.isNotNull(fixed, "FA-06: create_fixed must succeed");
    Assert.isTrue(fixed->page_count >= 3,
                  "FA-06: page_count must reflect requested page count (got %zu)",
                  fixed->page_count);

    // Allocate to near-end of the third page
    usize alloc_count = 0;
    while (Allocator.Scope.alloc(fixed, 256) != NULL) {
        alloc_count++;
    }
    // 3 pages × 8192 / 256 = 96 allocs (rounded down after alignment)
    Assert.isTrue(alloc_count >= 90, "FA-06: must serve many allocs across 3 pages (got %zu)",
                  alloc_count);
    Allocator.Arena.dispose(fixed);
}

#endif
/* ══════════════════════════════════════════════════════════════════════════ */
#if 1  // Region: FA-07 — cursor-save frame (bump rewind)

/**
 * FA-07: frame_begin saves the bump cursor; allocs within the frame are visible
 *        until frame_end restores the cursor (all frame bytes reclaimed).
 */
void test_fa_07_frame_cursor_save(void) {
    scope fixed = Allocator.Arena.create_fixed("fa07", 4096);
    Assert.isNotNull(fixed, "FA-07: create_fixed must succeed");

    object pre = Allocator.Scope.alloc(fixed, 64);
    Assert.isNotNull(pre, "FA-07: pre-frame alloc must succeed");

    frame f = Allocator.Arena.frame_begin(fixed);
    Assert.isNotNull(f, "FA-07: frame_begin on FIXED arena must succeed");

    object in1 = Allocator.Scope.alloc(fixed, 128);
    object in2 = Allocator.Scope.alloc(fixed, 64);
    Assert.isNotNull(in1, "FA-07: in-frame alloc 1 must succeed");
    Assert.isNotNull(in2, "FA-07: in-frame alloc 2 must succeed");

    // Cursor must have advanced
    Assert.isTrue(fixed->slab_bump > 64, "FA-07: slab_bump must have advanced during frame");

    integer rc = Allocator.Arena.frame_end(fixed, f);
    Assert.isTrue(rc == OK, "FA-07: frame_end must succeed");

    // Cursor restored: next alloc lands at same address as in1
    object after = Allocator.Scope.alloc(fixed, 128);
    Assert.isTrue(after == in1, "FA-07: alloc after frame_end must reuse reclaimed region");
    (void)pre;
    Allocator.Arena.dispose(fixed);
}

#endif
/* ══════════════════════════════════════════════════════════════════════════ */
#if 1  // Region: FA-08 — dispose restores R7 and unmaps slab

/**
 * FA-08: Disposing a fixed arena restores R7 to SLB0 and clears the scope slot.
 */
void test_fa_08_dispose(void) {
    // Save R7 at test-start (sigma.test may have it pointing to its own scope)
    scope pre_create = Allocator.Scope.current();
    scope fixed = Allocator.Arena.create_fixed("fa08", 4096);
    Assert.isNotNull(fixed, "FA-08: create_fixed must succeed");
    Assert.isTrue(Allocator.Scope.current() == fixed,
                  "FA-08: R7 must be fixed arena before dispose");

    // Capture slot index so we can verify it clears
    usize slot_id = fixed->scope_id;

    Allocator.Arena.dispose(fixed);

    // Dispose must restore R7 to whatever was active before create_fixed
    Assert.isTrue(Allocator.Scope.current() == pre_create,
                  "FA-08: R7 must be restored to pre-create scope after dispose");

    // Slot must be cleared (scope_id == 0 and nodepool_base == ADDR_EMPTY)
    scope recycled = Memory.get_scope(slot_id);
    Assert.isTrue(recycled->scope_id == 0, "FA-08: scope_table slot must be cleared after dispose");
}

#endif
/* ══════════════════════════════════════════════════════════════════════════ */
#if 1  // Region: FA-09 — NULL / zero-capacity guards

/**
 * FA-09: create_fixed with zero capacity returns NULL.
 *        Scope.alloc with size 0 returns NULL.
 */
void test_fa_09_guards(void) {
    scope bad = Allocator.Arena.create_fixed("fa09z", 0);
    Assert.isNull(bad, "FA-09: create_fixed(0) must return NULL");

    scope valid = Allocator.Arena.create_fixed("fa09v", 4096);
    Assert.isNotNull(valid, "FA-09: create_fixed with valid capacity must succeed");

    object p = Allocator.Scope.alloc(valid, 0);
    Assert.isNull(p, "FA-09: alloc(size=0) must return NULL");
    Allocator.Arena.dispose(valid);
}

#endif
/* ══════════════════════════════════════════════════════════════════════════ */
#if 1  // Region: Test Runner

__attribute__((constructor)) void init_test(void) {
    testset("Arena: SCOPE_POLICY_FIXED bump arena (FT-16)", set_config, set_teardown);
    testcase("FA-01: create_fixed + basic alloc", test_fa_01_create_and_alloc);
    testcase("FA-02: capacity enforced — NULL past slab end", test_fa_02_capacity_enforced);
    testcase("FA-03: bump contiguity — adjacent allocs", test_fa_03_bump_contiguity);
    testcase("FA-04: 16-byte alignment invariant", test_fa_04_alignment);
    testcase("FA-05: no NodePool allocated", test_fa_05_no_nodepool);
    testcase("FA-06: multi-page slab (3 × 8 KB)", test_fa_06_multi_page);
    testcase("FA-07: cursor-save frame (bump rewind)", test_fa_07_frame_cursor_save);
    testcase("FA-08: dispose restores R7 and clears slot", test_fa_08_dispose);
    testcase("FA-09: NULL / zero-capacity guards", test_fa_09_guards);
}

#endif
