/*
 * Test File: test/unit/test_bootstrap.c
 * Test Set:  Memory: SYS0 Bootstrap
 * Tags:      BST-01 .. BST-05
 *
 * Verifies that init_memory_system() correctly:
 *   - mmaps and aligns the 8 KB SYS0 page
 *   - fixes R7 to SLB0 (never SYS0 base)
 *   - embeds and initialises sc_ctrl_registry_s in SYS0
 *   - registers SLB0 as the sole controller at startup
 *   - provides a working Allocator.alloc/free facade via SLB0
 *
 * RED STATE: links against memory.c which does not exist yet.
 *            All tests will fail at link time until Phase 1 implementation.
 */

#include "internal/memory.h"
#include "memory.h"
// ----
#include <sigma.test/sigtest.h>

#if 1  // Region: Test Set Setup & Teardown
static void set_config(FILE **log_stream) {
    *log_stream = fopen("logs/test_bootstrap.log", "w");
}

static void set_teardown(void) {
}
#endif

#if 1  // Region: BST — SYS0 Bootstrap tests

// BST-01: SYS0 mmap'd at correct size
void test_bst01_sys0_size(void) {
    usize actual = memory_sys0_size();
    Assert.isTrue(actual == SYS0_PAGE_SIZE, "BST-01: SYS0 size should be %zu, got %zu",
                  (usize)SYS0_PAGE_SIZE, actual);
}

// BST-02: SYS0 base address is kAlign-aligned
void test_bst02_sys0_alignment(void) {
    addr base = memory_sys0_base();
    Assert.isNotNull((void *)base, "BST-02: SYS0 base must not be NULL");
    Assert.isTrue((base % kAlign) == 0, "BST-02: SYS0 base 0x%lx is not %u-byte aligned", base,
                  kAlign);
}

// BST-03: R7 is non-null and distinct from SYS0 base (R7 == SLB0 ctrl, not SYS0)
void test_bst03_r7_fixed_to_slb0(void) {
    addr r7 = memory_r7();
    addr sys0 = memory_sys0_base();
    Assert.isNotNull((void *)r7, "BST-03: R7 must not be NULL after bootstrap");
    Assert.isTrue(r7 != sys0,
                  "BST-03: R7 (0x%lx) must differ from SYS0 base (0x%lx) — "
                  "R7 is SLB0, not SYS0",
                  r7, sys0);
}

// BST-04: registry embedded in SYS0; count == 1; slot 0 == POLICY_RECLAIM
void test_bst04_registry_slb0_registered(void) {
    sc_ctrl_registry_s *reg = memory_registry();
    Assert.isNotNull(reg, "BST-04: registry pointer must not be NULL");
    Assert.isTrue(reg->count == 1, "BST-04: registry count should be 1 after bootstrap, got %u",
                  reg->count);
    Assert.isNotNull(reg->entries[0], "BST-04: registry slot 0 (SLB0 ctrl) must not be NULL");
    Assert.isTrue(reg->entries[0]->policy == POLICY_RECLAIM,
                  "BST-04: SLB0 ctrl policy should be POLICY_RECLAIM (%d), got %d", POLICY_RECLAIM,
                  reg->entries[0]->policy);
}

// BST-05: SLB0 ctrl pointer matches R7 (they are the same object)
void test_bst05_r7_equals_registry_slot0(void) {
    addr r7 = memory_r7();
    sc_ctrl_registry_s *reg = memory_registry();
    Assert.isNotNull(reg, "BST-05: registry must not be NULL");
    Assert.isTrue(r7 == (addr)reg->entries[0],
                  "BST-05: R7 (0x%lx) must equal registry->entries[0] (0x%lx)", r7,
                  (addr)reg->entries[0]);
}

// BST-06: Allocator.alloc / free round-trip (basic SLB0 sanity)
void test_bst06_alloc_free_roundtrip(void) {
    object p = Allocator.alloc(64);
    Assert.isNotNull(p, "BST-06: Allocator.alloc(64) must return non-NULL");
    Allocator.dispose(p);
}

#endif

#if 1  // Region: Test Registration
static void register_bootstrap_tests(void) {
    testset("Memory: SYS0 Bootstrap", set_config, set_teardown);
    testcase("BST-01: SYS0 size", test_bst01_sys0_size);
    testcase("BST-02: SYS0 alignment", test_bst02_sys0_alignment);
    testcase("BST-03: R7 fixed to SLB0", test_bst03_r7_fixed_to_slb0);
    testcase("BST-04: registry SLB0 registered", test_bst04_registry_slb0_registered);
    testcase("BST-05: R7 == registry slot 0", test_bst05_r7_equals_registry_slot0);
    testcase("BST-06: Allocator.alloc/free round-trip", test_bst06_alloc_free_roundtrip);
}

__attribute__((constructor)) static void init_bootstrap_tests(void) {
    Tests.enqueue(register_bootstrap_tests);
}
#endif
