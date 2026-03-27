/*
 * Test File: test/unit/test_facade.c
 * Test Set:  Memory: Facade (Phase 5)
 * Tags:      FAC-01 .. FAC-06
 *
 * Verifies the Allocator public facade — drop-in compat surface for v0.2.x callers:
 *   FAC-01: Allocator.alloc dispatches to SLB0 — returns non-NULL, memory writable
 *   FAC-02: Allocator.dispose — alloc then dispose, stable (valgrind catches leaks)
 *   FAC-03: Allocator.realloc(NULL, n) — NULL-ptr first arg acts as alloc
 *   FAC-04: Allocator.realloc(ptr, n2) — data preserved on grow
 *   FAC-05: Allocator.acquire(size) — returns slab with correct geometry
 *   FAC-06: create_bump round-trip via Allocator — alloc, reset, release
 */

#include "internal/memory.h"
#include "memory.h"
// ----
#include <sigma.test/sigtest.h>
#include <string.h>

#if 1  // Region: Test Set Setup & Teardown
static void set_config(FILE **log_stream) {
    *log_stream = fopen("logs/test_facade.log", "w");
}
static void set_teardown(void) {
}
#endif

#if 1  // Region: FAC — Facade tests

// FAC-01: Allocator.alloc(n) dispatches to SLB0 — returns non-NULL, memory accessible
void test_fac01_alloc_returns_non_null(void) {
    object p = Allocator.alloc(128);
    Assert.isNotNull(p, "FAC-01: Allocator.alloc(128) must return non-NULL");

    // Confirm the memory is writable and readable
    memset(p, 0xAB, 128);
    Assert.isTrue(((uint8_t *)p)[0] == 0xAB, "FAC-01: written byte must be readable back");

    Allocator.dispose(p);
}

// FAC-02: Allocator.dispose — allocate, write, dispose; must not crash; no leaks under valgrind
void test_fac02_free_stable(void) {
    object p = Allocator.alloc(256);
    Assert.isNotNull(p, "FAC-02: alloc must succeed before dispose");

    memset(p, 0x00, 256);
    Allocator.dispose(p);

    // If we get here without a crash, the test passes.
    // Valgrind validates zero leaks.
    Assert.isTrue(true, "FAC-02: dispose completed without crash");
}

// FAC-03: Allocator.realloc(NULL, n) — NULL first arg acts as alloc
void test_fac03_realloc_null_is_alloc(void) {
    object p = Allocator.realloc(NULL, 64);
    Assert.isNotNull(p, "FAC-03: realloc(NULL, 64) must return non-NULL");

    memset(p, 0x55, 64);
    Assert.isTrue(((uint8_t *)p)[0] == 0x55, "FAC-03: returned memory must be writable");

    Allocator.dispose(p);
}

// FAC-04: Allocator.realloc(ptr, n2) — first n bytes of data preserved after grow
void test_fac04_realloc_preserves_data(void) {
    const usize initial = 64;
    const usize grown = 256;

    object p = Allocator.alloc(initial);
    Assert.isNotNull(p, "FAC-04: initial alloc must succeed");
    memset(p, 0x42, initial);

    object p2 = Allocator.realloc(p, grown);
    Assert.isNotNull(p2, "FAC-04: realloc grow must succeed");

    bool preserved = true;
    for (usize i = 0; i < initial; i++) {
        if (((uint8_t *)p2)[i] != 0x42) {
            preserved = false;
            break;
        }
    }
    Assert.isTrue(preserved, "FAC-04: first %zu bytes must be preserved after grow", initial);

    Allocator.dispose(p2);
}

// FAC-05: Allocator.acquire(size) — returns non-NULL slab with correct geometry
void test_fac05_acquire_slab_geometry(void) {
    const usize sz = 4096;
    slab s = Allocator.acquire(sz);

    Assert.isNotNull(s, "FAC-05: acquire(4096) must return non-NULL slab");
    Assert.isNotNull(s->base, "FAC-05: slab base must be non-NULL");
    Assert.isTrue(s->size == sz, "FAC-05: slab size must equal requested size (%zu)", sz);
    Assert.isTrue(s->slab_id != 0, "FAC-05: slab_id must be non-zero after acquire");

    // slb_release_raw munmaps the backing and slb0_frees the sc_slab_s descriptor
    slb_release_raw(s);
}

// FAC-06: create_bump round-trip via Allocator — alloc, verify non-overlap, reset, release
void test_fac06_create_bump_roundtrip(void) {
    bump_allocator a = Allocator.create_bump(8192);
    Assert.isNotNull(a, "FAC-06: create_bump(8192) must return non-NULL");
    Assert.isTrue(a->base.policy == POLICY_BUMP, "FAC-06: policy must be POLICY_BUMP");

    object p1 = a->alloc(a, 128);
    object p2 = a->alloc(a, 128);
    Assert.isNotNull(p1, "FAC-06: first alloc on bump must succeed");
    Assert.isNotNull(p2, "FAC-06: second alloc on bump must succeed");
    Assert.isTrue(p1 != p2, "FAC-06: successive allocations must not overlap");

    a->reset(a, false);
    Assert.isTrue(a->cursor == 0, "FAC-06: reset must zero the cursor");

    Allocator.release((sc_ctrl_base_s *)a);
}

#endif  // Region: FAC — Facade tests

#if 1  // Region: Test Registration
static void register_facade_tests(void) {
    testset("Memory: Facade", set_config, set_teardown);
    testcase("FAC-01: alloc non-NULL, writable", test_fac01_alloc_returns_non_null);
    testcase("FAC-02: free is stable", test_fac02_free_stable);
    testcase("FAC-03: realloc(NULL,n) is alloc", test_fac03_realloc_null_is_alloc);
    testcase("FAC-04: realloc preserves data", test_fac04_realloc_preserves_data);
    testcase("FAC-05: acquire slab geometry", test_fac05_acquire_slab_geometry);
    testcase("FAC-06: create_bump round-trip", test_fac06_create_bump_roundtrip);
}

__attribute__((constructor)) static void init_facade_tests(void) {
    Tests.enqueue(register_facade_tests);
}
#endif
