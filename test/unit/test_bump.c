/*
 * Test File: test/unit/test_bump.c
 * Test Set:  Memory: Bump Controller (Phase 2)
 * Tags:      BC-01 .. BC-12
 *
 * Verifies the sc_bump_ctrl_s lifecycle:
 *   - alloc within / at / over capacity
 *   - reset (cursor-only and zero-fill)
 *   - frame_begin / frame_end restore
 *   - nested frames
 *   - frame stack overflow
 *   - kAlign alignment guarantee
 *
 * RED STATE: allocator_create_bump not yet implemented.
 */

#include "internal/memory.h"
#include "sigma.memory/memory.h"
// ----
#include <sigma.test/sigtest.h>

#if 1  // Region: Test Set Setup & Teardown
static void set_config(FILE **log_stream) {
    *log_stream = fopen("logs/test_bump.log", "w");
}
static void set_teardown(void) {
}
#endif

#if 1  // Region: BC — Bump Controller tests

// BC-01: alloc within capacity returns non-NULL, distinct pointers
void test_bc01_alloc_within_capacity(void) {
    bump_allocator a = Allocator.create_bump(4096);
    Assert.isNotNull(a, "BC-01: create_bump must return non-NULL");

    object p1 = a->alloc(a, 64);
    object p2 = a->alloc(a, 64);
    Assert.isNotNull(p1, "BC-01: first alloc must succeed");
    Assert.isNotNull(p2, "BC-01: second alloc must succeed");
    Assert.isTrue(p1 != p2, "BC-01: allocations must not overlap");

    Allocator.release((sc_ctrl_base_s *)a);
}

// BC-02: alloc exactly fills remaining capacity
void test_bc02_alloc_exact_capacity(void) {
    bump_allocator a = Allocator.create_bump(4096);
    Assert.isNotNull(a, "BC-02: create_bump must succeed");

    object p = a->alloc(a, a->capacity);
    Assert.isNotNull(p, "BC-02: alloc(capacity) must succeed");

    object over = a->alloc(a, 1);
    Assert.isNull(over, "BC-02: alloc after full capacity must return NULL");

    Allocator.release((sc_ctrl_base_s *)a);
}

// BC-03: alloc over capacity returns NULL
void test_bc03_alloc_over_capacity(void) {
    bump_allocator a = Allocator.create_bump(4096);
    Assert.isNotNull(a, "BC-03: create_bump must succeed");

    object p = a->alloc(a, a->capacity + 1);
    Assert.isNull(p, "BC-03: alloc(capacity+1) must return NULL");

    Allocator.release((sc_ctrl_base_s *)a);
}

// BC-04: reset brings cursor back to 0; slab reusable
void test_bc04_reset_reuse(void) {
    bump_allocator a = Allocator.create_bump(4096);
    Assert.isNotNull(a, "BC-04: create_bump must succeed");

    object p1 = a->alloc(a, 256);
    Assert.isNotNull(p1, "BC-04: initial alloc must succeed");

    a->reset(a, false);
    Assert.isTrue(a->cursor == 0, "BC-04: cursor must be 0 after reset");

    object p2 = a->alloc(a, 256);
    Assert.isNotNull(p2, "BC-04: post-reset alloc must succeed");
    Assert.isTrue(p1 == p2, "BC-04: post-reset alloc must return same base address");

    Allocator.release((sc_ctrl_base_s *)a);
}

// BC-05: reset(zero=true) zeroes memory
void test_bc05_reset_zero_fill(void) {
    bump_allocator a = Allocator.create_bump(4096);
    Assert.isNotNull(a, "BC-05: create_bump must succeed");

    uint8_t *p = a->alloc(a, 256);
    Assert.isNotNull(p, "BC-05: alloc must succeed");
    for (usize i = 0; i < 256; i++) p[i] = 0xAB;

    a->reset(a, true);

    uint8_t *p2 = a->alloc(a, 256);
    Assert.isNotNull(p2, "BC-05: post-reset alloc must succeed");
    bool zeroed = true;
    for (usize i = 0; i < 256; i++) {
        if (p2[i] != 0) {
            zeroed = false;
            break;
        }
    }
    Assert.isTrue(zeroed, "BC-05: memory must be zero after reset(true)");

    Allocator.release((sc_ctrl_base_s *)a);
}

// BC-06: frame_begin / frame_end — allocations after frame_begin are rolled back
void test_bc06_frame_roundtrip(void) {
    bump_allocator a = Allocator.create_bump(4096);
    Assert.isNotNull(a, "BC-06: create_bump must succeed");

    object pre = a->alloc(a, 64);
    Assert.isNotNull(pre, "BC-06: pre-frame alloc must succeed");
    usize cursor_before = a->cursor;

    frame f = a->frame_begin(a);
    Assert.isTrue(f != FRAME_NULL, "BC-06: frame_begin must return non-null frame");

    object in_frame = a->alloc(a, 128);
    Assert.isNotNull(in_frame, "BC-06: in-frame alloc must succeed");

    a->frame_end(a, f);
    Assert.isTrue(a->cursor == cursor_before,
                  "BC-06: cursor must return to pre-frame value after frame_end");

    Allocator.release((sc_ctrl_base_s *)a);
}

// BC-07: nested frames — each level rolls back independently
void test_bc07_nested_frames(void) {
    bump_allocator a = Allocator.create_bump(4096);
    Assert.isNotNull(a, "BC-07: create_bump must succeed");

    frame f1 = a->frame_begin(a);
    a->alloc(a, 64);
    usize after_f1_alloc = a->cursor;

    frame f2 = a->frame_begin(a);
    a->alloc(a, 64);

    frame f3 = a->frame_begin(a);
    a->alloc(a, 64);

    frame f4 = a->frame_begin(a);
    a->alloc(a, 64);

    a->frame_end(a, f4);
    a->frame_end(a, f3);
    a->frame_end(a, f2);
    Assert.isTrue(a->cursor == after_f1_alloc,
                  "BC-07: cursor must equal post-f1-alloc position after f2/f3/f4 rollback");

    a->frame_end(a, f1);
    Assert.isTrue(a->cursor == 0, "BC-07: cursor must be 0 after f1 rollback");

    Allocator.release((sc_ctrl_base_s *)a);
}

// BC-08: frame_begin at depth SC_FRAME_DEPTH_MAX returns FRAME_NULL
void test_bc08_frame_overflow(void) {
    bump_allocator a = Allocator.create_bump(65536);
    Assert.isNotNull(a, "BC-08: create_bump must succeed");

    frame frames[SC_FRAME_DEPTH_MAX];
    for (int i = 0; i < SC_FRAME_DEPTH_MAX; i++) {
        frames[i] = a->frame_begin(a);
        Assert.isTrue(frames[i] != FRAME_NULL, "BC-08: frame %d must succeed (depth <= max)", i);
    }

    frame overflow = a->frame_begin(a);
    Assert.isTrue(overflow == FRAME_NULL, "BC-08: frame_begin at depth %d must return FRAME_NULL",
                  SC_FRAME_DEPTH_MAX);

    for (int i = SC_FRAME_DEPTH_MAX - 1; i >= 0; i--) a->frame_end(a, frames[i]);

    Allocator.release((sc_ctrl_base_s *)a);
}

// BC-09: frame_end restores cursor precisely
void test_bc09_frame_end_restore(void) {
    bump_allocator a = Allocator.create_bump(4096);
    Assert.isNotNull(a, "BC-09: create_bump must succeed");

    a->alloc(a, 32);
    usize snap = a->cursor;

    frame f = a->frame_begin(a);
    a->alloc(a, 32);
    a->alloc(a, 64);
    a->alloc(a, 128);

    a->frame_end(a, f);
    Assert.isTrue(a->cursor == snap, "BC-09: cursor must be %zu after frame_end, got %zu", snap,
                  a->cursor);

    Allocator.release((sc_ctrl_base_s *)a);
}

// BC-10: returned pointers are kAlign-aligned
void test_bc10_alignment(void) {
    bump_allocator a = Allocator.create_bump(4096);
    Assert.isNotNull(a, "BC-10: create_bump must succeed");

    for (usize sz = 1; sz <= 128; sz++) {
        object p = a->alloc(a, sz);
        if (!p) break;
        Assert.isTrue(((uintptr_t)p % kAlign) == 0, "BC-10: alloc(%zu) pointer not %d-aligned", sz,
                      kAlign);
    }

    Allocator.release((sc_ctrl_base_s *)a);
}

// BC-11: create_bump(NULL) returns NULL
void test_bc11_create_bump_null_slab(void) {
    bump_allocator a = Allocator.create_bump(0);
    Assert.isNull(a, "BC-11: create_bump(NULL) must return NULL");
}

// BC-12: alloc(0) returns NULL
void test_bc12_alloc_zero(void) {
    bump_allocator a = Allocator.create_bump(4096);
    Assert.isNotNull(a, "BC-12: create_bump must succeed");

    object p = a->alloc(a, 0);
    Assert.isNull(p, "BC-12: alloc(0) must return NULL");

    Allocator.release((sc_ctrl_base_s *)a);
}

#endif

#if 1  // Region: Test Registration
static void register_bump_tests(void) {
    testset("Memory: Bump Controller", set_config, set_teardown);
    testcase("BC-01: alloc within capacity", test_bc01_alloc_within_capacity);
    testcase("BC-02: alloc exact capacity", test_bc02_alloc_exact_capacity);
    testcase("BC-03: alloc over capacity → NULL", test_bc03_alloc_over_capacity);
    testcase("BC-04: reset reuse", test_bc04_reset_reuse);
    testcase("BC-05: reset zero-fill", test_bc05_reset_zero_fill);
    testcase("BC-06: frame round-trip", test_bc06_frame_roundtrip);
    testcase("BC-07: nested frames (depth 4)", test_bc07_nested_frames);
    testcase("BC-08: frame overflow → NULL", test_bc08_frame_overflow);
    testcase("BC-09: frame_end restores cursor", test_bc09_frame_end_restore);
    testcase("BC-10: kAlign alignment", test_bc10_alignment);
    testcase("BC-11: create_bump(NULL) → NULL", test_bc11_create_bump_null_slab);
    testcase("BC-12: alloc(0) → NULL", test_bc12_alloc_zero);
}

__attribute__((constructor)) static void init_bump_tests(void) {
    Tests.enqueue(register_bump_tests);
}
#endif
