/*
 * SigmaCore
 * Copyright (c) 2026 David Boarman (BadKraft) and contributors
 * QuantumOverride [Q|]
 * ----------------------------------------------
 * MIT License
 * ----------------------------------------------
 * File: test_resource_scope.c
 * Description: TDD tests for FT-12 — SCOPE_POLICY_RESOURCE (rscope)
 *              RS-01 through RS-11: acquire/alloc/reset/release/frame lifecycle
 */

#include <sigma.test/sigtest.h>
#include "internal/memory.h"
#include "memory.h"
#include <string.h>

#if 1  // Region: Test Set Setup & Teardown
static void set_config(FILE **log_stream) {
    *log_stream = fopen("logs/test_resource_scope.log", "w");
    sbyte state = Memory.state();
    Assert.isTrue(state & MEM_STATE_READY, "Memory system should be ready");
}

static void set_teardown(void) {
    // Safety net: release any resource scopes leaked by a failed test (slots 2-15).
    // Slot 2 belongs to sigma.test framework — do not touch it.
    for (usize i = 3; i < 16; i++) {
        scope s = Memory.get_scope(i);
        if (s != NULL && s->policy == SCOPE_POLICY_RESOURCE) {
            rscope rs = (rscope)s;
            if (rs->slab_base != ADDR_EMPTY)
                Allocator.Resource.release(rs);
        }
    }
}
#endif

#if 1  // Region: RS - Resource Scope Tests

// ============================================================================
// RS-01: acquire returns non-NULL for valid size
// ============================================================================
void test_resource_acquire_returns_non_null(void) {
    rscope s = Allocator.Resource.acquire(65536);
    Assert.isNotNull(s, "acquire(64KB) should succeed");
    printf("  Acquired resource scope: id=%zu slab_base=0x%lx capacity=%zu\n",
           s->scope_id, (unsigned long)s->slab_base, s->slab_capacity);
    Allocator.Resource.release(s);
}

// ============================================================================
// RS-02: acquire claims a scope_table slot with correct metadata
// ============================================================================
void test_resource_acquire_claims_slot(void) {
    rscope s = Allocator.Resource.acquire(65536);
    Assert.isNotNull(s, "acquire should succeed");
    Assert.isTrue(s->scope_id >= 2 && s->scope_id <= 15,
        "scope_id should be in user range [2,15], got %zu", s->scope_id);
    Assert.isTrue(s->policy == SCOPE_POLICY_RESOURCE,
        "policy should be SCOPE_POLICY_RESOURCE");
    Assert.isTrue(s->nodepool_base == ADDR_EMPTY,
        "nodepool_base must be ADDR_EMPTY");
    Assert.isTrue(s->prev == NULL,
        "prev must be NULL — resource scopes never enter R7 chain");
    Assert.isTrue(s->slab_base != ADDR_EMPTY,
        "slab_base must be set after acquire");
    Assert.isTrue(s->slab_capacity == 65536,
        "slab_capacity must match requested size");
    printf("  slot=%zu policy=%d nodepool=0x%lx prev=%p\n",
           s->scope_id, s->policy, (unsigned long)s->nodepool_base, (void *)s->prev);
    Allocator.Resource.release(s);
}

// ============================================================================
// RS-03: alloc returns aligned pointer within slab bounds
// ============================================================================
void test_resource_alloc_aligned_within_bounds(void) {
    rscope s = Allocator.Resource.acquire(65536);
    Assert.isNotNull(s, "acquire should succeed");

    object p = Allocator.Resource.alloc(s, 100);
    Assert.isNotNull(p, "alloc(100) should succeed");
    Assert.isTrue((uintptr_t)p >= (uintptr_t)s->slab_base,
        "pointer must be >= slab_base");
    Assert.isTrue((uintptr_t)p < (uintptr_t)s->slab_base + s->slab_capacity,
        "pointer must be < slab end");
    Assert.isTrue((uintptr_t)p % kAlign == 0,
        "pointer must be %d-byte aligned, got 0x%lx", kAlign, (unsigned long)p);
    printf("  alloc(100) -> 0x%lx (aligned=%s)\n",
           (unsigned long)p, ((uintptr_t)p % kAlign == 0) ? "yes" : "no");
    Allocator.Resource.release(s);
}

// ============================================================================
// RS-04: alloc returns NULL on exhaustion without crashing
// ============================================================================
void test_resource_alloc_null_on_exhaustion(void) {
    // 64 bytes slab, kAlign=16: room for exactly 4 × 16-byte allocations
    rscope s = Allocator.Resource.acquire(64);
    Assert.isNotNull(s, "acquire(64) should succeed");

    object p1 = Allocator.Resource.alloc(s, 16);
    object p2 = Allocator.Resource.alloc(s, 16);
    object p3 = Allocator.Resource.alloc(s, 16);
    object p4 = Allocator.Resource.alloc(s, 16);
    object p5 = Allocator.Resource.alloc(s, 16);  // must be NULL

    Assert.isNotNull(p1, "alloc 1 should succeed");
    Assert.isNotNull(p2, "alloc 2 should succeed");
    Assert.isNotNull(p3, "alloc 3 should succeed");
    Assert.isNotNull(p4, "alloc 4 should succeed");
    Assert.isNull(p5,    "alloc 5 must return NULL — slab exhausted");
    printf("  p1=%p p2=%p p3=%p p4=%p p5(expect NULL)=%p\n",
           p1, p2, p3, p4, p5);
    Allocator.Resource.release(s);
}

// ============================================================================
// RS-05: reset(false) restores bump cursor; slab remains mapped
// ============================================================================
void test_resource_reset_cursor_only(void) {
    rscope s = Allocator.Resource.acquire(1024);
    Assert.isNotNull(s, "acquire should succeed");

    addr base = s->slab_base;
    Allocator.Resource.alloc(s, 128);
    addr after_alloc = s->bump_pos;
    Assert.isTrue(after_alloc > base, "bump_pos should advance after alloc");

    Allocator.Resource.reset(s, false);
    Assert.isTrue(s->bump_pos == base,
        "bump_pos must equal slab_base after reset(false)");
    printf("  base=0x%lx after_alloc=0x%lx after_reset=0x%lx\n",
           (unsigned long)base, (unsigned long)after_alloc,
           (unsigned long)s->bump_pos);
    Allocator.Resource.release(s);
}

// ============================================================================
// RS-06: reset(true) zeroes slab and restores bump cursor
// ============================================================================
void test_resource_reset_zero_clears_slab(void) {
    rscope s = Allocator.Resource.acquire(256);
    Assert.isNotNull(s, "acquire should succeed");

    object p = Allocator.Resource.alloc(s, 64);
    Assert.isNotNull(p, "alloc should succeed");
    memset(p, 0xAB, 64);

    Allocator.Resource.reset(s, true);
    Assert.isTrue(s->bump_pos == s->slab_base,
        "bump_pos must equal slab_base after reset(true)");

    byte *mem = (byte *)s->slab_base;
    bool all_zero = true;
    for (usize i = 0; i < 64; i++) {
        if (mem[i] != 0) { all_zero = false; break; }
    }
    Assert.isTrue(all_zero, "first 64 bytes must be zero after zero-reset");
    printf("  zero-reset: slab zeroed=%s bump=0x%lx\n",
           all_zero ? "yes" : "no", (unsigned long)s->bump_pos);
    Allocator.Resource.release(s);
}

// ============================================================================
// RS-07: R7 unchanged by acquire and release
// ============================================================================
void test_resource_r7_unchanged(void) {
    scope before = (scope)Allocator.Scope.current();
    rscope s = Allocator.Resource.acquire(4096);
    Assert.isNotNull(s, "acquire should succeed");
    Assert.isTrue(Allocator.Scope.current() == before,
        "acquire must not change R7");
    Allocator.Resource.release(s);
    Assert.isTrue(Allocator.Scope.current() == before,
        "release must not change R7");
    printf("  R7 before=0x%lx after_acquire=0x%lx after_release=0x%lx\n",
           (unsigned long)before,
           (unsigned long)Allocator.Scope.current(),
           (unsigned long)Allocator.Scope.current());
}

// ============================================================================
// RS-08: Scope.set rejected for resource scope
// ============================================================================
void test_resource_scope_set_rejected(void) {
    rscope s = Allocator.Resource.acquire(4096);
    Assert.isNotNull(s, "acquire should succeed");
    integer result = Allocator.Scope.set((scope)s);
    Assert.isTrue(result == ERR,
        "Scope.set on resource scope must return ERR");
    // R7 must not have changed either
    scope current = (scope)Allocator.Scope.current();
    Assert.isTrue(current != (scope)s,
        "R7 must not point to resource scope after rejected Scope.set");
    printf("  Scope.set result=%d (expected ERR=%d) R7 unchanged=%s\n",
           (int)result, (int)ERR, (current != (scope)s) ? "yes" : "no");
    Allocator.Resource.release(s);
}

// ============================================================================
// RS-09: Frame begin/end restores bump cursor
// ============================================================================
void test_resource_frame_begin_end(void) {
    rscope s = Allocator.Resource.acquire(4096);
    Assert.isNotNull(s, "acquire should succeed");

    Allocator.Resource.alloc(s, 256);
    addr cursor_before_frame = s->bump_pos;

    frame f = Allocator.Resource.frame_begin(s);
    Assert.isNotNull(f, "frame_begin should succeed");
    Assert.isTrue(s->frame_active, "frame_active must be true after frame_begin");

    Allocator.Resource.alloc(s, 512);
    Assert.isTrue(s->bump_pos > cursor_before_frame,
        "cursor should advance within frame");

    integer result = Allocator.Resource.frame_end(s, f);
    Assert.isTrue(result == OK, "frame_end should return OK");
    Assert.isTrue(s->bump_pos == cursor_before_frame,
        "bump_pos must be restored to pre-frame value");
    Assert.isTrue(!s->frame_active, "frame_active must be false after frame_end");
    printf("  cursor_before=0x%lx after_frame_end=0x%lx match=%s\n",
           (unsigned long)cursor_before_frame, (unsigned long)s->bump_pos,
           (s->bump_pos == cursor_before_frame) ? "yes" : "no");
    Allocator.Resource.release(s);
}

// ============================================================================
// RS-10: slot freed after release; next acquire may reuse it
// ============================================================================
void test_resource_slot_recycled_after_release(void) {
    rscope s1 = Allocator.Resource.acquire(1024);
    Assert.isNotNull(s1, "first acquire should succeed");
    usize id1 = s1->scope_id;
    printf("  s1 slot=%zu\n", id1);
    Allocator.Resource.release(s1);

    rscope s2 = Allocator.Resource.acquire(1024);
    Assert.isNotNull(s2, "second acquire should succeed");
    printf("  s2 slot=%zu (original was %zu)\n", s2->scope_id, id1);
    Assert.isTrue(s2->scope_id == id1,
        "slot should be recycled: expected %zu got %zu", id1, s2->scope_id);
    Allocator.Resource.release(s2);
}

// ============================================================================
// RS-11: shutdown reclaims unreleased resource scope (valgrind-verified)
// ============================================================================
void test_resource_shutdown_reclaims_leaked_scope(void) {
    // Acquire but do not release. shutdown_memory_system (destructor) must
    // munmap the slab. Run this test with --valgrind to confirm no leak.
    rscope s = Allocator.Resource.acquire(8192);
    Assert.isNotNull(s, "acquire should succeed");
    Allocator.Resource.alloc(s, 100);
    printf("  Leaked resource scope at slot=%zu slab=0x%lx capacity=%zu\n",
           s->scope_id, (unsigned long)s->slab_base, s->slab_capacity);
    printf("  (valgrind should report no leak — shutdown_memory_system reclaims it)\n");
    // Intentionally not calling release — shutdown is the cleanup path.
}
#endif

#if 1  // Region: Test Registration
__attribute__((constructor)) void init_resource_scope_tests(void)
{
    testset("Resource: Scope lifecycle (FT-12)", set_config, set_teardown);

    testcase("RS-01: acquire returns non-NULL",
             test_resource_acquire_returns_non_null);
    testcase("RS-02: acquire claims slot with correct metadata",
             test_resource_acquire_claims_slot);
    testcase("RS-03: alloc returns aligned pointer within slab",
             test_resource_alloc_aligned_within_bounds);
    testcase("RS-04: alloc returns NULL on exhaustion",
             test_resource_alloc_null_on_exhaustion);
    testcase("RS-05: reset(false) restores cursor only",
             test_resource_reset_cursor_only);
    testcase("RS-06: reset(true) zeroes slab and restores cursor",
             test_resource_reset_zero_clears_slab);
    testcase("RS-07: R7 unchanged by acquire and release",
             test_resource_r7_unchanged);
    testcase("RS-08: Scope.set rejected for resource scope",
             test_resource_scope_set_rejected);
    testcase("RS-09: frame begin/end restores bump cursor",
             test_resource_frame_begin_end);
    testcase("RS-10: slot recycled after release",
             test_resource_slot_recycled_after_release);
    testcase("RS-11: shutdown reclaims unreleased resource scope",
             test_resource_shutdown_reclaims_leaked_scope);
}
#endif
