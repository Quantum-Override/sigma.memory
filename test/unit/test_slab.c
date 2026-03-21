/*
 * Test File: test/unit/test_slab.c
 * Test Set:  Memory: Slab Type (Phase 1)
 * Tags:      SLB-01 .. SLB-04
 *
 * Verifies the raw slab lifecycle:
 *   - Allocator.acquire(size) mmaps a region and returns a populated sc_slab_s
 *   - acquired slabs receive unique non-zero slab_ids
 *   - Allocator.acquire(0) returns NULL
 *   - slb_release_raw(s) cleanly unmaps; Valgrind shows 0 bytes leaked
 *
 * RED STATE: links against memory.c which does not exist yet.
 *            All tests will fail at link time until Phase 1 implementation.
 */

#include "internal/memory.h"
#include "sigma.memory/memory.h"
// ----
#include <sigma.test/sigtest.h>

#if 1  // Region: Test Set Setup & Teardown
static void set_config(FILE **log_stream) {
    *log_stream = fopen("logs/test_slab.log", "w");
}

static void set_teardown(void) {
}
#endif

#if 1  // Region: SLB — Slab lifecycle tests

// SLB-01: acquire 4 KB — basic properties
void test_slb01_acquire_4k(void) {
    slab s = Allocator.acquire(4096);
    Assert.isNotNull(s, "SLB-01: acquire(4096) must return non-NULL slab");
    Assert.isNotNull(s->base, "SLB-01: slab->base must not be NULL");
    Assert.isTrue(s->size == 4096, "SLB-01: slab->size should be 4096, got %zu", s->size);
    Assert.isTrue(s->slab_id != 0, "SLB-01: slab_id must be non-zero (registered in SYS0)");
    slb_release_raw(s);
}

// SLB-02: acquire 8 MB — larger mmap still works
void test_slb02_acquire_8mb(void) {
    slab s = Allocator.acquire(8 * 1024 * 1024);
    Assert.isNotNull(s, "SLB-02: acquire(8MB) must return non-NULL slab");
    Assert.isNotNull(s->base, "SLB-02: slab->base must not be NULL");
    Assert.isTrue(s->size == 8 * 1024 * 1024, "SLB-02: slab->size should be 8MB, got %zu", s->size);
    slb_release_raw(s);
}

// SLB-03: acquire zero bytes returns NULL
void test_slb03_acquire_zero(void) {
    slab s = Allocator.acquire(0);
    Assert.isNull(s, "SLB-03: acquire(0) must return NULL");
}

// SLB-04: successive acquires get distinct slab_ids
void test_slb04_unique_slab_ids(void) {
    slab a = Allocator.acquire(4096);
    slab b = Allocator.acquire(4096);
    Assert.isNotNull(a, "SLB-04: first acquire must succeed");
    Assert.isNotNull(b, "SLB-04: second acquire must succeed");
    Assert.isTrue(a->slab_id != b->slab_id, "SLB-04: slab_ids must be distinct (%u vs %u)",
                  a->slab_id, b->slab_id);
    slb_release_raw(a);
    slb_release_raw(b);
}

#endif

#if 1  // Region: Test Registration
static void register_slab_tests(void) {
    testset("Memory: Slab Type", set_config, set_teardown);
    testcase("SLB-01: acquire 4KB slab", test_slb01_acquire_4k);
    testcase("SLB-02: acquire 8MB slab", test_slb02_acquire_8mb);
    testcase("SLB-03: acquire(0) → NULL", test_slb03_acquire_zero);
    testcase("SLB-04: unique slab_ids", test_slb04_unique_slab_ids);
}

__attribute__((constructor)) static void init_slab_tests(void) {
    Tests.enqueue(register_slab_tests);
}
#endif
