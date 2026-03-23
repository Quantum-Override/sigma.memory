/*
 * Test File: test/unit/test_trusted_app.c
 * Test Set:  Memory: Trusted-App Subsystem (FT-14)
 * Tags:      FTA-01 .. FTA-04
 *
 * Verifies the app-tier trusted grant mechanism (SIGMA_ROLE_TRUSTED_APP):
 *   FTA-01  trusted_app_grant returns non-NULL cap with populated alloc_use
 *   FTA-02  app slot does NOT overlap Ring1 slots (reg_slot <= TRUSTED_SLOT_MAX
 *           is NOT a valid constraint; app slots are counted independently)
 *   FTA-03  alloc/write/free through alloc_use — memory is valid, valgrind clean
 *   FTA-04  overflow beyond TRUSTED_APP_SLOT_MAX returns NULL
 *
 * Lifecycle note:
 *   The memory system is initialised ONCE by sigma_module_init_all() before all
 *   tests run.  Individual test cases MUST NOT call init_memory_system() or
 *   cleanup_memory_system().  App-slot state is reset in set_teardown via
 *   memory_trusted_app_reset(), which only unmaps the isolated app arenas and
 *   resets the app slot counter — R1–R6 and SLB0 are untouched.
 *
 *   Shared state:
 *     s_cap1 — granted in set_config (slot 1); used by FTA-01..03
 *     FTA-04 — fills slots 2..8, then probes for overflow
 */

#include "internal/memory.h"
#include "memory.h"
// ----
#include <sigma.test/sigtest.h>
#include <string.h>

// Shared cap established in set_config; valid for FTA-01..03.
static sc_trusted_cap_t *s_cap1 = NULL;

#if 1  // Region: Test Set Setup & Teardown
static void set_config(FILE **log_stream) {
    *log_stream = fopen("logs/test_trusted_app.log", "w");
    s_cap1 = trusted_app_grant("test.app.module.a", 0, POLICY_RECLAIM);
}

static void set_teardown(void) {
    memory_trusted_app_reset();
    s_cap1 = NULL;
}
#endif

#if 1  // Region: FTA — Trusted-app capability tests

// FTA-01: grant returns non-NULL cap with populated alloc_use hooks
void test_fta01_grant_not_null(void) {
    Assert.isNotNull(s_cap1, "FTA-01: trusted_app_grant must return non-NULL cap");
    Assert.isNotNull(s_cap1->alloc_use.alloc, "FTA-01: alloc_use.alloc must be wired");
    Assert.isNotNull(s_cap1->alloc_use.release, "FTA-01: alloc_use.release must be wired");
    Assert.isNotNull(s_cap1->alloc_use.resize, "FTA-01: alloc_use.resize must be wired");
}

// FTA-02: first app slot is 1 and arena is distinct from SLB0 (not Ring1)
void test_fta02_slot_independent_of_ring1(void) {
    Assert.isNotNull(s_cap1, "FTA-02: cap must be non-NULL");
    Assert.isTrue(s_cap1->reg_slot == 1, "FTA-02: first app-tier grant must receive slot 1, got %u",
                  (unsigned)s_cap1->reg_slot);
    Assert.isNotNull(s_cap1->arena, "FTA-02: cap->arena must not be NULL");
    Assert.isNotNull(s_cap1->arena->base, "FTA-02: arena->base must not be NULL");

    // The arena base must differ from SLB0 ctrl (R7)
    addr slb0_ctrl = memory_r7();
    Assert.isTrue((addr)s_cap1->arena->base != slb0_ctrl,
                  "FTA-02: app arena base must differ from SLB0 ctrl");

    // Verify app slot lookup works and returns same pointer
    sc_trusted_cap_t *looked_up = memory_trusted_app_cap(s_cap1->reg_slot);
    Assert.isTrue(looked_up == s_cap1,
                  "FTA-02: memory_trusted_app_cap(slot) must return the same cap pointer");
}

// FTA-03: alloc/write/free through alloc_use is functional and valgrind-clean
void test_fta03_alloc_use_works(void) {
    Assert.isNotNull(s_cap1, "FTA-03: cap must be non-NULL");

    void *ptr = s_cap1->alloc_use.alloc(128);
    Assert.isNotNull(ptr, "FTA-03: alloc(128) through cap->alloc_use must return non-NULL");

    memset(ptr, 0xCD, 128);
    uint8_t *bytes = (uint8_t *)ptr;
    Assert.isTrue(bytes[0] == 0xCD && bytes[127] == 0xCD,
                  "FTA-03: allocated memory must be writable and readable");

    s_cap1->alloc_use.release(ptr);  // must not crash
}

// FTA-04: overflow beyond TRUSTED_APP_SLOT_MAX returns NULL
void test_fta04_overflow_returns_null(void) {
    // Slot 1 is taken (set_config).  Fill 2..TRUSTED_APP_SLOT_MAX then overflow.
    for (uint8_t i = 2; i <= TRUSTED_APP_SLOT_MAX; i++) {
        sc_trusted_cap_t *cap = trusted_app_grant("test.app.overflow", 0, POLICY_RECLAIM);
        Assert.isNotNull(cap, "FTA-04: slot %u grant must succeed", (unsigned)i);
    }

    sc_trusted_cap_t *overflow = trusted_app_grant("test.app.overflow.extra", 0, POLICY_RECLAIM);
    Assert.isNull(overflow, "FTA-04: grant beyond TRUSTED_APP_SLOT_MAX must return NULL");
}

#endif

#if 1  // Region: Test Registration
static void register_trusted_app_tests(void) {
    testset("Memory: Trusted-App Subsystem", set_config, set_teardown);
    testcase("FTA-01: grant returns non-NULL cap with alloc_use", test_fta01_grant_not_null);
    testcase("FTA-02: app slot independent of Ring1 slots", test_fta02_slot_independent_of_ring1);
    testcase("FTA-03: alloc_use hooks functional (valgrind clean)", test_fta03_alloc_use_works);
    testcase("FTA-04: overflow beyond slot max returns NULL", test_fta04_overflow_returns_null);
}

__attribute__((constructor)) static void init_trusted_app_tests(void) {
    Tests.enqueue(register_trusted_app_tests);
}
#endif
