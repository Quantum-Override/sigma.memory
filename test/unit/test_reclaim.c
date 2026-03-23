/*
 * Test File: test/unit/test_reclaim.c
 * Test Set:  Memory: Reclaim Controller (Phase 3)
 * Tags:      RC-01 .. RC-14
 *
 * Verifies the sc_reclaim_ctrl_s lifecycle (Option-B first-fit free list):
 *   - alloc / free / realloc basic correctness
 *   - free-list coalescing (address-ordered)
 *   - realloc in-place shrink and grow paths
 *   - frame_begin / frame_end sequence-tag sweep
 *   - frame preserves pre-frame allocations
 *   - create_reclaim(NULL) → NULL
 *   - alloc(0) → NULL
 *
 * RED STATE: allocator_create_reclaim not yet implemented.
 */

#include "internal/memory.h"
#include "memory.h"
// ----
#include <sigma.test/sigtest.h>
#include <string.h>

#if 1  // Region: Test Set Setup & Teardown
static void set_config(FILE **log_stream) {
    *log_stream = fopen("logs/test_reclaim.log", "w");
}
static void set_teardown(void) {
}
#endif

#if 1  // Region: RC — Reclaim Controller tests

// RC-01: basic alloc returns non-NULL aligned pointer
void test_rc01_alloc_basic(void) {
    reclaim_allocator r = Allocator.create_reclaim(65536);
    Assert.isNotNull(r, "RC-01: create_reclaim must succeed");

    object p = r->alloc(r, 64);
    Assert.isNotNull(p, "RC-01: alloc(64) must return non-NULL");
    Assert.isTrue(((uintptr_t)p % kAlign) == 0, "RC-01: returned pointer must be %d-aligned",
                  kAlign);

    Allocator.release((sc_ctrl_base_s *)r);
}

// RC-02: alloc(0) → NULL
void test_rc02_alloc_zero(void) {
    reclaim_allocator r = Allocator.create_reclaim(65536);
    Assert.isNotNull(r, "RC-02: create_reclaim must succeed");

    object p = r->alloc(r, 0);
    Assert.isNull(p, "RC-02: alloc(0) must return NULL");

    Allocator.release((sc_ctrl_base_s *)r);
}

// RC-03: free then re-alloc same size reuses the block
void test_rc03_free_reuse(void) {
    reclaim_allocator r = Allocator.create_reclaim(65536);
    Assert.isNotNull(r, "RC-03: create_reclaim must succeed");

    object p1 = r->alloc(r, 64);
    Assert.isNotNull(p1, "RC-03: first alloc must succeed");
    r->free(r, p1);

    object p2 = r->alloc(r, 64);
    Assert.isNotNull(p2, "RC-03: second alloc after free must succeed");
    Assert.isTrue(p1 == p2, "RC-03: second alloc must reuse freed block");

    Allocator.release((sc_ctrl_base_s *)r);
}

// RC-04: adjacent freed blocks are coalesced
void test_rc04_coalesce(void) {
    reclaim_allocator r = Allocator.create_reclaim(65536);
    Assert.isNotNull(r, "RC-04: create_reclaim must succeed");

    object a = r->alloc(r, 64);
    object b = r->alloc(r, 64);
    Assert.isNotNull(a, "RC-04: alloc a must succeed");
    Assert.isNotNull(b, "RC-04: alloc b must succeed");

    r->free(r, a);
    r->free(r, b);

    // After coalescing, a single alloc of 128+ bytes should fit where a+b were
    object c = r->alloc(r, 128);
    Assert.isNotNull(c, "RC-04: alloc(128) after coalesce of two 64-byte blocks must succeed");
    Assert.isTrue(c == a, "RC-04: coalesced block must start at address of first freed block");

    Allocator.release((sc_ctrl_base_s *)r);
}

// RC-05: realloc shrink stays in-place
void test_rc05_realloc_shrink(void) {
    reclaim_allocator r = Allocator.create_reclaim(65536);
    Assert.isNotNull(r, "RC-05: create_reclaim must succeed");

    object p = r->alloc(r, 128);
    Assert.isNotNull(p, "RC-05: alloc(128) must succeed");

    object q = r->realloc(r, p, 64);
    Assert.isNotNull(q, "RC-05: realloc shrink must succeed");
    Assert.isTrue(q == p, "RC-05: shrink must be in-place");

    Allocator.release((sc_ctrl_base_s *)r);
}

// RC-06: realloc grow allocates new block and copies data
void test_rc06_realloc_grow(void) {
    reclaim_allocator r = Allocator.create_reclaim(65536);
    Assert.isNotNull(r, "RC-06: create_reclaim must succeed");

    object p = r->alloc(r, 32);
    Assert.isNotNull(p, "RC-06: initial alloc must succeed");
    memset(p, 0xAB, 32);

    object q = r->realloc(r, p, 128);
    Assert.isNotNull(q, "RC-06: realloc grow must succeed");

    // First 32 bytes must be preserved
    uint8_t *bytes = (uint8_t *)q;
    bool data_ok = true;
    for (int i = 0; i < 32; i++)
        if (bytes[i] != 0xAB) {
            data_ok = false;
            break;
        }
    Assert.isTrue(data_ok, "RC-06: realloc grow must preserve original data");

    Allocator.release((sc_ctrl_base_s *)r);
}

// RC-07: realloc(NULL) behaves as alloc
void test_rc07_realloc_null(void) {
    reclaim_allocator r = Allocator.create_reclaim(65536);
    Assert.isNotNull(r, "RC-07: create_reclaim must succeed");

    object p = r->realloc(r, NULL, 64);
    Assert.isNotNull(p, "RC-07: realloc(NULL, 64) must behave as alloc(64)");

    Allocator.release((sc_ctrl_base_s *)r);
}

// RC-08: frame_begin / frame_end — post-frame allocs are swept, pre-frame survive
void test_rc08_frame_sweep(void) {
    reclaim_allocator r = Allocator.create_reclaim(65536);
    Assert.isNotNull(r, "RC-08: create_reclaim must succeed");

    object pre = r->alloc(r, 64);
    Assert.isNotNull(pre, "RC-08: pre-frame alloc must succeed");

    frame f = r->frame_begin(r);
    Assert.isTrue(f != FRAME_NULL, "RC-08: frame_begin must return non-null frame");

    object in1 = r->alloc(r, 64);
    object in2 = r->alloc(r, 32);
    Assert.isNotNull(in1, "RC-08: in-frame alloc 1 must succeed");
    Assert.isNotNull(in2, "RC-08: in-frame alloc 2 must succeed");

    r->frame_end(r, f);

    // Pre-frame block must still be reachable (not swept)
    // We test indirectly: alloc a same-size block — should NOT equal pre (pre is live)
    object post = r->alloc(r, 64);
    Assert.isNotNull(post, "RC-08: post-frame-end alloc must succeed");
    Assert.isTrue(post != pre, "RC-08: pre-frame block must survive frame_end");

    Allocator.release((sc_ctrl_base_s *)r);
}

// RC-09: frame_end reclaims post-frame memory for immediate reuse
void test_rc09_frame_reuse(void) {
    reclaim_allocator r = Allocator.create_reclaim(65536);
    Assert.isNotNull(r, "RC-09: create_reclaim must succeed");

    frame f = r->frame_begin(r);
    object p = r->alloc(r, 128);
    Assert.isNotNull(p, "RC-09: in-frame alloc must succeed");
    r->frame_end(r, f);

    // Memory swept by frame_end should be reusable
    object q = r->alloc(r, 128);
    Assert.isNotNull(q, "RC-09: alloc after frame_end must reuse swept block");
    Assert.isTrue(q == p, "RC-09: reused address must match swept in-frame block");

    Allocator.release((sc_ctrl_base_s *)r);
}

// RC-10: multiple independent allocs all distinct, non-overlapping
void test_rc10_no_overlap(void) {
    reclaim_allocator r = Allocator.create_reclaim(65536);
    Assert.isNotNull(r, "RC-10: create_reclaim must succeed");

#define RC10_N 8
    object ptrs[RC10_N];
    for (int i = 0; i < RC10_N; i++) {
        ptrs[i] = r->alloc(r, 64);
        Assert.isNotNull(ptrs[i], "RC-10: alloc %d must succeed", i);
    }

    // Verify no two pointers are the same
    for (int i = 0; i < RC10_N; i++)
        for (int j = i + 1; j < RC10_N; j++)
            Assert.isTrue(ptrs[i] != ptrs[j], "RC-10: ptrs[%d] and ptrs[%d] must be distinct", i,
                          j);

    Allocator.release((sc_ctrl_base_s *)r);
#undef RC10_N
}

// RC-11: alloc writes data, free does not corrupt adjacent blocks
void test_rc11_isolation(void) {
    reclaim_allocator r = Allocator.create_reclaim(65536);
    Assert.isNotNull(r, "RC-11: create_reclaim must succeed");

    object a = r->alloc(r, 64);
    object b = r->alloc(r, 64);
    Assert.isNotNull(a, "RC-11: alloc a");
    Assert.isNotNull(b, "RC-11: alloc b");

    memset(a, 0xAA, 64);
    memset(b, 0xBB, 64);

    r->free(r, a);

    // b must be untouched
    uint8_t *bp = (uint8_t *)b;
    bool ok = true;
    for (int i = 0; i < 64; i++)
        if (bp[i] != 0xBB) {
            ok = false;
            break;
        }
    Assert.isTrue(ok, "RC-11: freeing a must not corrupt b");

    Allocator.release((sc_ctrl_base_s *)r);
}

// RC-12: returned pointers are kAlign-aligned for varied sizes
void test_rc12_alignment(void) {
    reclaim_allocator r = Allocator.create_reclaim(65536);
    Assert.isNotNull(r, "RC-12: create_reclaim must succeed");

    for (usize sz = 1; sz <= 256; sz += 7) {
        object p = r->alloc(r, sz);
        if (!p) break;
        Assert.isTrue(((uintptr_t)p % kAlign) == 0, "RC-12: alloc(%zu) pointer not %d-aligned", sz,
                      kAlign);
        r->free(r, p);
    }

    Allocator.release((sc_ctrl_base_s *)r);
}

// RC-13: create_reclaim(NULL) → NULL
void test_rc13_null_slab(void) {
    reclaim_allocator r = Allocator.create_reclaim(0);
    Assert.isNull(r, "RC-13: create_reclaim(NULL) must return NULL");
}

// RC-14: nested frame_begin / frame_end — inner frame swept, outer frame survives
void test_rc14_nested_frames(void) {
    reclaim_allocator r = Allocator.create_reclaim(65536);
    Assert.isNotNull(r, "RC-14: create_reclaim must succeed");

    frame f1 = r->frame_begin(r);
    object p1 = r->alloc(r, 64);
    Assert.isTrue(f1 != FRAME_NULL, "RC-14: outer frame_begin must succeed");
    Assert.isNotNull(p1, "RC-14: outer frame alloc must succeed");

    frame f2 = r->frame_begin(r);
    object p2 = r->alloc(r, 64);
    Assert.isTrue(f2 != FRAME_NULL, "RC-14: inner frame_begin must succeed");
    Assert.isNotNull(p2, "RC-14: inner frame alloc must succeed");

    r->frame_end(r, f2);  // sweep only inner (p2)

    // p1 must still be live — inner frame_end must not sweep outer frame allocs
    // Verify by allocating 64-byte block: if it equals p2 the inner was swept;
    // it must NOT equal p1 (outer is live).
    object reuse = r->alloc(r, 64);
    Assert.isNotNull(reuse, "RC-14: post-inner-frame alloc must succeed");
    Assert.isTrue(reuse != p1, "RC-14: outer-frame alloc must survive inner frame_end");
    Assert.isTrue(reuse == p2, "RC-14: inner-frame block must be reused after frame_end");

    r->frame_end(r, f1);  // sweep outer (p1)

    Allocator.release((sc_ctrl_base_s *)r);
}

#endif  // Region: RC — Reclaim Controller tests

#if 1  // Region: Test Registration
static void register_reclaim_tests(void) {
    testset("Memory: Reclaim Controller", set_config, set_teardown);
    testcase("RC-01: alloc basic", test_rc01_alloc_basic);
    testcase("RC-02: alloc(0) → NULL", test_rc02_alloc_zero);
    testcase("RC-03: free reuse", test_rc03_free_reuse);
    testcase("RC-04: coalesce", test_rc04_coalesce);
    testcase("RC-05: realloc shrink", test_rc05_realloc_shrink);
    testcase("RC-06: realloc grow", test_rc06_realloc_grow);
    testcase("RC-07: realloc(NULL)", test_rc07_realloc_null);
    testcase("RC-08: frame sweep", test_rc08_frame_sweep);
    testcase("RC-09: frame reuse", test_rc09_frame_reuse);
    testcase("RC-10: no overlap", test_rc10_no_overlap);
    testcase("RC-11: isolation", test_rc11_isolation);
    testcase("RC-12: alignment", test_rc12_alignment);
    testcase("RC-13: create_reclaim(NULL)", test_rc13_null_slab);
    testcase("RC-14: nested frames", test_rc14_nested_frames);
}

__attribute__((constructor)) static void init_reclaim_tests(void) {
    Tests.enqueue(register_reclaim_tests);
}
#endif
