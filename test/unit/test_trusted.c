/*
 * Test File: test/unit/test_trusted.c
 * Test Set:  Memory: Trusted Subsystem (FT-11)
 * Tags:      TRS-01 .. TRS-07
 *
 * Verifies the Ring0/Ring1 trusted grant mechanism:
 *   TRS-01  trusted_grant returns non-NULL cap for a valid request
 *   TRS-02  cap->reg_slot == 1 for the first granted module
 *   TRS-03  memory_trusted_cap(slot) returns the same cap pointer; ctrl is non-NULL
 *   TRS-04  cap->arena is non-NULL and distinct from SLB0 (R7)
 *   TRS-05  alloc_use hooks work: alloc 64 bytes, write, free
 *   TRS-06  second grant receives slot 2, arena distinct from first cap
 *   TRS-07  grant beyond TRUSTED_SLOT_MAX returns NULL
 *
 * Lifecycle note:
 *   The memory system is initialised ONCE by sigma_module_init_all() before all
 *   tests run.  Individual test cases MUST NOT call init_memory_system() or
 *   cleanup_memory_system() because sigma.test's HookRegistry lives in SLB0.
 *   Trusted-slot state is reset in set_teardown via memory_trusted_reset() which
 *   only unmaps the isolated trusted arenas and resets the slot counter.
 *
 *   Shared state:
 *     s_cap1 — granted in set_config (slot 1); used by TRS-01..05
 *     s_cap2 — granted inside TRS-06 (slot 2)
 *     TRS-07 — fills slots 2..6 directly, then probes for overflow
 */

#include "internal/memory.h"
#include "memory.h"
// ----
#include <sigma.test/sigtest.h>
#include <string.h>

// Shared cap established in set_config; valid for TRS-01..05.
static sc_trusted_cap_t *s_cap1 = NULL;

#if 1  // Region: Test Set Setup & Teardown
static void set_config(FILE **log_stream) {
    *log_stream = fopen("logs/test_trusted.log", "w");
    // Grant the first trusted slot so TRS-01..05 can inspect it immediately.
    s_cap1 = trusted_grant("test.module.a", 0, POLICY_RECLAIM);
}

static void set_teardown(void) {
    // Release all trusted arenas and reset the slot counter.
    // Does NOT touch SLB0 — sigma.test's HookRegistry remains valid.
    memory_trusted_reset();
    s_cap1 = NULL;
}
#endif

#if 1  // Region: TRS — Trusted capability tests

// TRS-01: grant returns non-NULL cap for a valid request
void test_trs01_grant_not_null(void) {
    Assert.isNotNull(s_cap1, "TRS-01: trusted_grant must return non-NULL cap");
}

// TRS-02: first granted module receives slot 1
void test_trs02_first_slot_is_one(void) {
    Assert.isNotNull(s_cap1, "TRS-02: cap must be non-NULL");
    Assert.isTrue(s_cap1->reg_slot == 1, "TRS-02: first trusted module must receive slot 1, got %u",
                  s_cap1->reg_slot);
}

// TRS-03: memory_trusted_cap(slot) returns the same pointer; ctrl is non-NULL
void test_trs03_register_stamped(void) {
    Assert.isNotNull(s_cap1, "TRS-03: cap must be non-NULL");
    sc_trusted_cap_t *looked_up = memory_trusted_cap(s_cap1->reg_slot);
    Assert.isTrue(looked_up == s_cap1,
                  "TRS-03: memory_trusted_cap(slot) must return the same cap pointer");
    Assert.isNotNull(s_cap1->ctrl, "TRS-03: cap->ctrl must not be NULL");
}

// TRS-04: cap->arena is non-NULL and base is distinct from SLB0 ctrl (R7)
void test_trs04_arena_distinct_from_slb0(void) {
    Assert.isNotNull(s_cap1, "TRS-04: cap must be non-NULL");
    Assert.isNotNull(s_cap1->arena, "TRS-04: cap->arena must not be NULL");
    Assert.isNotNull(s_cap1->arena->base, "TRS-04: cap->arena->base must not be NULL");

    addr slb0_ctrl = memory_r7();
    Assert.isTrue((addr)s_cap1->arena->base != slb0_ctrl,
                  "TRS-04: trusted arena base must differ from SLB0 ctrl");
}

// TRS-05: alloc_use hooks work — alloc, write, free through the cap
void test_trs05_alloc_use_works(void) {
    Assert.isNotNull(s_cap1, "TRS-05: cap must be non-NULL");
    Assert.isNotNull(s_cap1->alloc_use.alloc, "TRS-05: alloc_use.alloc must be wired");
    Assert.isNotNull(s_cap1->alloc_use.release, "TRS-05: alloc_use.release must be wired");
    Assert.isNotNull(s_cap1->alloc_use.resize, "TRS-05: alloc_use.resize must be wired");

    void *ptr = s_cap1->alloc_use.alloc(64);
    Assert.isNotNull(ptr, "TRS-05: alloc(64) through cap->alloc_use must return non-NULL");

    memset(ptr, 0xAB, 64);
    uint8_t *bytes = (uint8_t *)ptr;
    Assert.isTrue(bytes[0] == 0xAB && bytes[63] == 0xAB,
                  "TRS-05: allocated memory must be writable and readable");

    s_cap1->alloc_use.release(ptr);  // must not crash
}

// TRS-06: second grant receives slot 2 with a distinct arena
void test_trs06_second_grant_slot_two(void) {
    Assert.isNotNull(s_cap1, "TRS-06: first cap must be non-NULL");

    sc_trusted_cap_t *cap2 = trusted_grant("test.module.b", 0, POLICY_RECLAIM);
    Assert.isNotNull(cap2, "TRS-06: second grant must succeed");
    Assert.isTrue(cap2->reg_slot == 2, "TRS-06: second cap must be slot 2, got %u", cap2->reg_slot);
    Assert.isTrue(s_cap1->arena->base != cap2->arena->base,
                  "TRS-06: the two trusted arenas must be distinct");
}

// TRS-07: fill remaining slots then verify overflow returns NULL
void test_trs07_overflow_returns_null(void) {
    // Slots 1 and 2 are taken (set_config + TRS-06).  Fill 3..6 then overflow.
    for (uint8_t i = 3; i <= TRUSTED_SLOT_MAX; i++) {
        sc_trusted_cap_t *cap = trusted_grant("test.overflow", 0, POLICY_RECLAIM);
        Assert.isNotNull(cap, "TRS-07: slot %u grant must succeed", (unsigned)i);
    }

    sc_trusted_cap_t *overflow = trusted_grant("test.overflow.extra", 0, POLICY_RECLAIM);
    Assert.isNull(overflow, "TRS-07: grant beyond TRUSTED_SLOT_MAX must return NULL");
}

#endif

#if 1  // Region: Test Registration
static void register_trusted_tests(void) {
    testset("Memory: Trusted Subsystem", set_config, set_teardown);
    testcase("TRS-01: grant returns non-NULL cap", test_trs01_grant_not_null);
    testcase("TRS-02: first slot is 1", test_trs02_first_slot_is_one);
    testcase("TRS-03: cap lookup and ctrl non-NULL", test_trs03_register_stamped);
    testcase("TRS-04: arena distinct from SLB0", test_trs04_arena_distinct_from_slb0);
    testcase("TRS-05: alloc_use hooks work", test_trs05_alloc_use_works);
    testcase("TRS-06: second grant gets slot 2", test_trs06_second_grant_slot_two);
    testcase("TRS-07: overflow beyond max returns NULL", test_trs07_overflow_returns_null);
}

__attribute__((constructor)) static void init_trusted_tests(void) {
    Tests.enqueue(register_trusted_tests);
}
#endif
