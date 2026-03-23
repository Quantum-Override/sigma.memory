/*
 * Test File: test/unit/test_registry.c
 * Test Set:  Memory: Registry (Phase 4)
 * Tags:      REG-01 .. REG-08
 *
 * Verifies the controller registry lifecycle:
 *   - create_bump / create_reclaim allocate internally and register
 *   - controllers appear in registry entries
 *   - release() deregisters and frees controller + backing
 *   - slot reuse after release
 *   - create_custom() with typed factory
 *   - registry capacity limit (31 user slots)
 */

#include "internal/memory.h"
#include "memory.h"
// ----
#include <sigma.test/sigtest.h>

#if 1  // Region: Test Set Setup & Teardown
static void set_config(FILE **log_stream) {
    *log_stream = fopen("logs/test_registry.log", "w");
}
static void set_teardown(void) {
}
#endif

#if 1  // Region: REG — Registry tests

// REG-01: create_bump returns non-NULL with populated backing slab
void test_reg01_create_bump_backed(void) {
    bump_allocator a = Allocator.create_bump(4096);
    Assert.isNotNull(a, "REG-01: create_bump(4096) must return non-NULL");
    Assert.isNotNull(a->base.backing, "REG-01: backing slab must be non-NULL");
    Assert.isNotNull(a->base.backing->base, "REG-01: backing slab base must be non-NULL");
    Assert.isTrue(a->base.backing->size == 4096, "REG-01: backing size must equal requested size");
    Allocator.release((sc_ctrl_base_s *)a);
}

// REG-02: create_reclaim returns non-NULL with populated backing slab
void test_reg02_create_reclaim_backed(void) {
    reclaim_allocator r = Allocator.create_reclaim(65536);
    Assert.isNotNull(r, "REG-02: create_reclaim(65536) must return non-NULL");
    Assert.isNotNull(r->base.backing, "REG-02: backing slab must be non-NULL");
    Assert.isNotNull(r->base.backing->base, "REG-02: backing slab base must be non-NULL");
    Assert.isTrue(r->base.backing->size == 65536, "REG-02: backing size must equal requested size");
    Allocator.release((sc_ctrl_base_s *)r);
}

// REG-03: created controllers appear in registry entries
void test_reg03_registry_entries(void) {
    sc_ctrl_registry_s *reg = memory_registry();
    uint8_t count_before = reg->count;

    bump_allocator a = Allocator.create_bump(4096);
    reclaim_allocator r = Allocator.create_reclaim(4096);
    Assert.isNotNull(a, "REG-03: create_bump must succeed");
    Assert.isNotNull(r, "REG-03: create_reclaim must succeed");

    // Both must appear somewhere in entries[1..count-1]
    bool found_a = false, found_r = false;
    for (uint8_t i = 1; i < reg->count; i++) {
        if (reg->entries[i] == (sc_ctrl_base_s *)a) found_a = true;
        if (reg->entries[i] == (sc_ctrl_base_s *)r) found_r = true;
    }
    Assert.isTrue(found_a, "REG-03: bump allocator must be in registry");
    Assert.isTrue(found_r, "REG-03: reclaim allocator must be in registry");
    Assert.isTrue(reg->count == count_before + 2,
                  "REG-03: count must increase by 2 after two creates");

    Allocator.release((sc_ctrl_base_s *)a);
    Allocator.release((sc_ctrl_base_s *)r);
}

// REG-04: release() NULLs the registry slot
void test_reg04_release_clears_slot(void) {
    sc_ctrl_registry_s *reg = memory_registry();

    bump_allocator a = Allocator.create_bump(4096);
    Assert.isNotNull(a, "REG-04: create_bump must succeed");

    // Find which slot it landed in
    uint8_t slot = 0;
    for (uint8_t i = 1; i < reg->count; i++) {
        if (reg->entries[i] == (sc_ctrl_base_s *)a) {
            slot = i;
            break;
        }
    }
    Assert.isTrue(slot > 0, "REG-04: must find bump allocator in registry");

    Allocator.release((sc_ctrl_base_s *)a);

    Assert.isNull(reg->entries[slot], "REG-04: slot must be NULL after release");
}

// REG-05: slot reuse — after release the slot is reused by the next create
void test_reg05_slot_reuse(void) {
    sc_ctrl_registry_s *reg = memory_registry();

    bump_allocator a = Allocator.create_bump(4096);
    Assert.isNotNull(a, "REG-05: first create must succeed");

    uint8_t slot = 0;
    for (uint8_t i = 1; i < SC_MAX_CONTROLLERS; i++) {
        if (reg->entries[i] == (sc_ctrl_base_s *)a) {
            slot = i;
            break;
        }
    }
    Assert.isTrue(slot > 0, "REG-05: must find first allocator in registry");

    Allocator.release((sc_ctrl_base_s *)a);
    Assert.isNull(reg->entries[slot], "REG-05: slot must be NULL after release");

    // Next create must reuse the freed slot (first-fit from slot 1)
    bump_allocator b = Allocator.create_bump(4096);
    Assert.isNotNull(b, "REG-05: second create must succeed");
    Assert.isTrue(reg->entries[slot] == (sc_ctrl_base_s *)b,
                  "REG-05: second create must land in the same slot");

    Allocator.release((sc_ctrl_base_s *)b);
}

// REG-06: create_custom with a minimal bump-compatible factory
static sc_ctrl_base_s *custom_factory(slab s) {
    bump_allocator a = Allocator.alloc(sizeof(struct sc_bump_ctrl_s));
    if (!a) return NULL;
    a->base.policy = POLICY_BUMP;
    a->base.backing = s;
    a->base.struct_size = sizeof(struct sc_bump_ctrl_s);
    a->base.external = false;
    a->base.shutdown = NULL;  // factory-created; we test registration, not production use
    a->cursor = 0;
    a->capacity = s->size & ~(usize)(kAlign - 1);
    a->frame_depth = 0;
    a->alloc = NULL;
    a->reset = NULL;
    a->frame_begin = NULL;
    a->frame_end = NULL;
    return (sc_ctrl_base_s *)a;
}

void test_reg06_create_custom(void) {
    sc_ctrl_registry_s *reg = memory_registry();

    sc_ctrl_base_s *ctrl = Allocator.create_custom(4096, custom_factory);
    Assert.isNotNull(ctrl, "REG-06: create_custom must return non-NULL");
    Assert.isTrue(ctrl->policy == POLICY_BUMP, "REG-06: custom ctrl must carry factory policy");

    bool found = false;
    for (uint8_t i = 1; i < reg->count; i++) {
        if (reg->entries[i] == ctrl) {
            found = true;
            break;
        }
    }
    Assert.isTrue(found, "REG-06: custom controller must be registered");

    Allocator.release(ctrl);
}

// REG-07: fill all 31 user slots — all creates succeed
void test_reg07_fill_registry(void) {
    sc_ctrl_registry_s *reg = memory_registry();
    bump_allocator handles[SC_MAX_CONTROLLERS - 1];  // slots 1..31
    int created = 0;

    for (int i = 0; i < SC_MAX_CONTROLLERS - 1; i++) {
        handles[i] = Allocator.create_bump(4096);
        if (!handles[i]) break;
        created++;
    }
    Assert.isTrue(created == SC_MAX_CONTROLLERS - 1,
                  "REG-07: must successfully fill all %d user slots, got %d",
                  SC_MAX_CONTROLLERS - 1, created);

    // Verify registry count reached max
    Assert.isTrue(reg->count == SC_MAX_CONTROLLERS,
                  "REG-07: registry count must equal SC_MAX_CONTROLLERS (%d)", SC_MAX_CONTROLLERS);

    for (int i = 0; i < created; i++) Allocator.release((sc_ctrl_base_s *)handles[i]);
}

// REG-08: 32nd create attempt after 31 filled → returns NULL
void test_reg08_registry_full(void) {
    bump_allocator handles[SC_MAX_CONTROLLERS - 1];
    int created = 0;

    for (int i = 0; i < SC_MAX_CONTROLLERS - 1; i++) {
        handles[i] = Allocator.create_bump(4096);
        if (!handles[i]) break;
        created++;
    }
    Assert.isTrue(created == SC_MAX_CONTROLLERS - 1,
                  "REG-08: setup: must fill all user slots, got %d", created);

    bump_allocator overflow = Allocator.create_bump(4096);
    Assert.isNull(overflow, "REG-08: create_bump past registry capacity must return NULL");

    for (int i = 0; i < created; i++) Allocator.release((sc_ctrl_base_s *)handles[i]);
}

#endif  // Region: REG — Registry tests

#if 1  // Region: Test Registration
static void register_registry_tests(void) {
    testset("Memory: Registry", set_config, set_teardown);
    testcase("REG-01: create_bump backed slab", test_reg01_create_bump_backed);
    testcase("REG-02: create_reclaim backed slab", test_reg02_create_reclaim_backed);
    testcase("REG-03: entries in registry", test_reg03_registry_entries);
    testcase("REG-04: release clears slot", test_reg04_release_clears_slot);
    testcase("REG-05: slot reuse after release", test_reg05_slot_reuse);
    testcase("REG-06: create_custom factory", test_reg06_create_custom);
    testcase("REG-07: fill all 31 user slots", test_reg07_fill_registry);
    testcase("REG-08: registry full → NULL", test_reg08_registry_full);
}

__attribute__((constructor)) static void init_registry_tests(void) {
    Tests.enqueue(register_registry_tests);
}
#endif
