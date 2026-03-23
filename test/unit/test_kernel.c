/*
 * Test File: test/unit/test_kernel.c
 * Test Set:  Memory: Kernel Controller (Phase 3B)
 * Tags:      KNL-01 .. KNL-10
 *
 * Verifies sc_kernel_ctrl_s — the MTIS-backed bootstrap controller that
 * underpins SLB0 (and future Ring0 kernel memory).
 *
 *   KNL-01  kernel ctrl lives in SYS0 DAT at the expected offset
 *   KNL-02  backing slab is exactly 2 MB
 *   KNL-03  kernel_alloc returns non-NULL for a valid size
 *   KNL-04  kernel_alloc(0) returns NULL
 *   KNL-05  freed block is reused by subsequent alloc (MTIS reclaim)
 *   KNL-06  hundreds of small allocs without exhaustion
 *   KNL-07  large alloc (> 4 KB page) succeeds
 *   KNL-08  nodepool is initialised (non-NULL, capacity == KNL_NODEPOOL_SIZE)
 *   KNL-09  allocated block appears in B-tree (alloc_count tracks correctly)
 *   KNL-10  coalesce: alloc A + B, free both, result is one coalesced block
 *
 * RED STATE: memory.c does not yet implement sc_kernel_ctrl_s.
 *            All tests fail at link or runtime until Phase 3B is done.
 */

#include "internal/memory.h"
#include "memory.h"
// ----
#include <sigma.test/sigtest.h>

#if 1  // Region: Test Set Setup & Teardown
static void set_config(FILE **log_stream) {
    *log_stream = fopen("logs/test_kernel.log", "w");
}

static void set_teardown(void) {
}
#endif

#if 1  // Region: KNL — Kernel Controller tests

// KNL-01: kernel ctrl is in SYS0 DAT at the expected offset
void test_knl01_ctrl_in_sys0(void) {
    sc_kernel_ctrl_s *k = memory_kernel_ctrl();
    addr sys0 = memory_sys0_base();
    Assert.isNotNull(k, "KNL-01: memory_kernel_ctrl() must return non-NULL");
    addr kaddr = (addr)k;
    Assert.isTrue(kaddr >= sys0 && kaddr < sys0 + memory_sys0_size(),
                  "KNL-01: kernel ctrl must reside within SYS0 page (got %p, sys0=%p)",
                  (void *)kaddr, (void *)sys0);
}

// KNL-02: kernel backing slab is 2 MB
void test_knl02_slab_size(void) {
    sc_kernel_ctrl_s *k = memory_kernel_ctrl();
    Assert.isNotNull(k, "KNL-02: memory_kernel_ctrl() must return non-NULL");
    Assert.isNotNull(k->base.backing, "KNL-02: kernel ctrl must have a backing slab");
    Assert.isTrue(k->base.backing->size == KNL_SLAB_SIZE,
                  "KNL-02: backing slab must be %u bytes, got %zu", KNL_SLAB_SIZE,
                  k->base.backing->size);
    Assert.isNotNull(k->base.backing->base, "KNL-02: slab base must be non-NULL");
}

// KNL-03: basic alloc returns non-NULL
void test_knl03_alloc_basic(void) {
    sc_kernel_ctrl_s *k = memory_kernel_ctrl();
    object ptr = k->alloc(k, 64);
    Assert.isNotNull(ptr, "KNL-03: kernel alloc(64) must return non-NULL");
    k->free(k, ptr);
}

// KNL-04: alloc(0) returns NULL
void test_knl04_alloc_zero(void) {
    sc_kernel_ctrl_s *k = memory_kernel_ctrl();
    object ptr = k->alloc(k, 0);
    Assert.isNull(ptr, "KNL-04: kernel alloc(0) must return NULL");
}

// KNL-05: freed block is reused (MTIS reclaim)
void test_knl05_free_reuse(void) {
    sc_kernel_ctrl_s *k = memory_kernel_ctrl();
    usize bump_before = k->bump;
    object a = k->alloc(k, 128);
    Assert.isNotNull(a, "KNL-05: first alloc must succeed");
    k->free(k, a);
    object b = k->alloc(k, 128);
    Assert.isNotNull(b, "KNL-05: second alloc after free must succeed");
    // The arena-level bump must not advance: reuse came from the free list, not a new page
    Assert.isTrue(k->bump == bump_before,
                  "KNL-05: bump must not advance when reusing a freed block (got %zu, want %zu)",
                  k->bump, bump_before);
    k->free(k, b);
}

// KNL-06: hundreds of small allocs without exhaustion
void test_knl06_many_allocs(void) {
    sc_kernel_ctrl_s *k = memory_kernel_ctrl();
    enum { N = 500 };
    object ptrs[N];
    for (int i = 0; i < N; i++) {
        ptrs[i] = k->alloc(k, 32);
        Assert.isNotNull(ptrs[i], "KNL-06: alloc %d of %d must succeed", i + 1, N);
    }
    for (int i = 0; i < N; i++) k->free(k, ptrs[i]);
}

// KNL-07: large alloc (> one page = 4 KB)
void test_knl07_large_alloc(void) {
    sc_kernel_ctrl_s *k = memory_kernel_ctrl();
    object ptr = k->alloc(k, 8192);
    Assert.isNotNull(ptr, "KNL-07: kernel alloc(8192) must succeed");
    k->free(k, ptr);
}

// KNL-08: nodepool is initialised
void test_knl08_nodepool_init(void) {
    sc_kernel_ctrl_s *k = memory_kernel_ctrl();
    Assert.isNotNull(k->nodepool, "KNL-08: nodepool must be non-NULL");
    Assert.isTrue(k->nodepool->capacity == KNL_NODEPOOL_SIZE,
                  "KNL-08: nodepool capacity must be %u, got %zu", KNL_NODEPOOL_SIZE,
                  k->nodepool->capacity);
}

// KNL-09: alloc_count on the page increments after alloc
void test_knl09_btree_tracking(void) {
    sc_kernel_ctrl_s *k = memory_kernel_ctrl();
    // skip_head points to the first page_node; after any alloc that page should
    // have alloc_count >= 1.
    object ptr = k->alloc(k, 64);
    Assert.isNotNull(ptr, "KNL-09: alloc must succeed");
    knl_nodepool_hdr_s *np = k->nodepool;
    uint16_t ph = np->skip_head;
    Assert.isTrue(ph != KNL_PAGE_NULL, "KNL-09: skip list must have at least one page");
    // Get page_node pointer: grows up from offset sizeof(knl_nodepool_hdr_s)
    knl_page_node_s *pn =
        (knl_page_node_s *)((uint8_t *)np + sizeof(knl_nodepool_hdr_s)) + (ph - 1);
    Assert.isTrue(pn->alloc_count >= 1, "KNL-09: page alloc_count must be >= 1 after alloc, got %u",
                  pn->alloc_count);
    k->free(k, ptr);
}

// KNL-10: coalesce — alloc A + B, free both → one coalesced free block
void test_knl10_coalesce(void) {
    sc_kernel_ctrl_s *k = memory_kernel_ctrl();
    usize bump_start = k->bump;
    object a = k->alloc(k, 64);
    object b = k->alloc(k, 64);
    Assert.isNotNull(a, "KNL-10: alloc A must succeed");
    Assert.isNotNull(b, "KNL-10: alloc B must succeed");
    k->free(k, a);
    k->free(k, b);
    // After coalescing, bump should not have advanced further and a third alloc
    // of combined size should succeed without bumping into new territory.
    usize bump_after_free = k->bump;
    object c = k->alloc(k, 128);
    Assert.isNotNull(c, "KNL-10: coalesced alloc(128) must succeed");
    Assert.isTrue(k->bump == bump_after_free,
                  "KNL-10: bump must not advance when reusing coalesced block");
    k->free(k, c);
    (void)bump_start;
}

#endif

#if 1  // Region: Test Registration
static void register_kernel_tests(void) {
    testset("Memory: Kernel Controller", set_config, set_teardown);
    testcase("KNL-01: ctrl in SYS0 DAT", test_knl01_ctrl_in_sys0);
    testcase("KNL-02: backing slab is 2 MB", test_knl02_slab_size);
    testcase("KNL-03: alloc(64) → non-NULL", test_knl03_alloc_basic);
    testcase("KNL-04: alloc(0) → NULL", test_knl04_alloc_zero);
    testcase("KNL-05: free + reuse", test_knl05_free_reuse);
    testcase("KNL-06: 500 small allocs", test_knl06_many_allocs);
    testcase("KNL-07: large alloc (8 KB)", test_knl07_large_alloc);
    testcase("KNL-08: nodepool initialised", test_knl08_nodepool_init);
    testcase("KNL-09: B-tree tracks allocation", test_knl09_btree_tracking);
    testcase("KNL-10: coalesce A+B free → reuse", test_knl10_coalesce);
}

__attribute__((constructor)) static void init_kernel_tests(void) {
    Tests.enqueue(register_kernel_tests);
}
#endif
