/*
 * SigmaCore
 * Copyright (c) 2026 David Boarman (BadKraft) and contributors
 * QuantumOverride [Q|]
 * ----------------------------------------------
 * MIT License
 * ----------------------------------------------
 * File: test_promote.c
 * Description: TDD tests for FT-14 — Scope.promote (cross-scope allocation promotion)
 *              PM-01 through PM-09: copy + lifetime promotion between all scope types
 */

#include <sigma.test/sigtest.h>
#include "internal/memory.h"
#include "memory.h"
#include <string.h>

#if 1  // Region: Test Set Setup & Teardown
static void set_config(FILE **log_stream) {
    *log_stream = fopen("logs/test_promote.log", "w");
    Allocator.Scope.restore();  // sigma.test arena at slot 2 is current; back to SLB0
    sbyte state = Memory.state();
    Assert.isTrue(state & MEM_STATE_READY, "Memory system should be ready");
}

static void set_teardown(void) {
    // Safety net: clean up any scopes leaked by a failed test (slots 3-15).
    for (usize i = 3; i < 16; i++) {
        scope s = Memory.get_scope(i);
        if (s == NULL) continue;
        if (s->policy == SCOPE_POLICY_RESOURCE) {
            rscope rs = (rscope)s;
            if (rs->slab_base != ADDR_EMPTY)
                Allocator.Resource.release(rs);
        } else if (s->nodepool_base != ADDR_EMPTY) {
            Allocator.Arena.dispose(s);
        }
    }
    Allocator.Scope.restore();
}
#endif

#if 1  // Region: PM - Promote Tests

// ============================================================================
// PM-01: promote from arena frame to SLB0 (promoted data survives frame_end)
// ============================================================================
void test_promote_frame_to_slb0(void) {
    scope arena = Allocator.Arena.create("pm01", SCOPE_POLICY_DYNAMIC);
    Assert.isNotNull(arena, "arena create should succeed");

    frame f = Allocator.Frame.begin();
    Assert.isNotNull(f, "frame_begin should succeed");

    byte *p = Allocator.alloc(32);
    Assert.isNotNull(p, "frame alloc should succeed");
    for (int i = 0; i < 32; i++) p[i] = (byte)(i & 0xFF);

    scope slb0 = Memory.get_scope(1);
    byte *promoted = Allocator.promote(p, 32, slb0);
    Assert.isNotNull(promoted, "promote to SLB0 should succeed");

    Allocator.Frame.end(f);  // p is now invalid; promoted must survive

    bool ok = true;
    for (int i = 0; i < 32; i++) {
        if (promoted[i] != (byte)(i & 0xFF)) { ok = false; break; }
    }
    Assert.isTrue(ok, "promoted data must be intact in SLB0 after frame_end");
    printf("  promoted@%p data intact=%s\n", (void *)promoted, ok ? "yes" : "no");

    Allocator.dispose(promoted);
    Allocator.Arena.dispose(arena);
}

// ============================================================================
// PM-02: promote from arena frame to rscope (data survives frame_end)
// ============================================================================
void test_promote_frame_to_rscope(void) {
    scope arena = Allocator.Arena.create("pm02", SCOPE_POLICY_DYNAMIC);
    Assert.isNotNull(arena, "arena create should succeed");

    rscope rs = Allocator.Resource.acquire(4096);
    Assert.isNotNull(rs, "rscope acquire should succeed");

    frame f = Allocator.Frame.begin();  // frame on arena (R7)
    Assert.isNotNull(f, "frame_begin should succeed");

    byte *p = Allocator.alloc(32);
    Assert.isNotNull(p, "frame alloc should succeed");
    for (int i = 0; i < 32; i++) p[i] = (byte)((32 - i) & 0xFF);

    byte *promoted = Allocator.promote(p, 32, (scope)rs);
    Assert.isNotNull(promoted, "promote to rscope should succeed");

    Allocator.Frame.end(f);  // p invalid

    bool ok = true;
    for (int i = 0; i < 32; i++) {
        if (promoted[i] != (byte)((32 - i) & 0xFF)) { ok = false; break; }
    }
    Assert.isTrue(ok, "promoted data must be intact in rscope after frame_end");
    printf("  promoted@%p in rscope slot=%zu data intact=%s\n",
           (void *)promoted, rs->scope_id, ok ? "yes" : "no");

    Allocator.Resource.release(rs);
    Allocator.Arena.dispose(arena);
}

// ============================================================================
// PM-03: promote from rscope to SLB0 (data survives rscope release)
// ============================================================================
void test_promote_rscope_to_slb0(void) {
    rscope rs = Allocator.Resource.acquire(1024);
    Assert.isNotNull(rs, "rscope acquire should succeed");

    byte *p = Allocator.Resource.alloc(rs, 64);
    Assert.isNotNull(p, "rscope alloc should succeed");
    for (int i = 0; i < 64; i++) p[i] = (byte)(0xAA ^ i);

    scope slb0 = Memory.get_scope(1);
    byte *promoted = Allocator.promote(p, 64, slb0);
    Assert.isNotNull(promoted, "promote to SLB0 should succeed");

    Allocator.Resource.release(rs);  // p now invalid

    bool ok = true;
    for (int i = 0; i < 64; i++) {
        if (promoted[i] != (byte)(0xAA ^ i)) { ok = false; break; }
    }
    Assert.isTrue(ok, "promoted data must be intact in SLB0 after rscope release");
    printf("  promoted@%p data intact=%s\n", (void *)promoted, ok ? "yes" : "no");

    Allocator.dispose(promoted);
}

// ============================================================================
// PM-04: promote from rscope to arena (data survives rscope release)
// ============================================================================
void test_promote_rscope_to_arena(void) {
    rscope rs = Allocator.Resource.acquire(1024);
    Assert.isNotNull(rs, "rscope acquire should succeed");

    byte *p = Allocator.Resource.alloc(rs, 48);
    Assert.isNotNull(p, "rscope alloc should succeed");
    for (int i = 0; i < 48; i++) p[i] = (byte)(0x55 ^ i);

    scope arena = Allocator.Arena.create("pm04", SCOPE_POLICY_DYNAMIC);
    Assert.isNotNull(arena, "arena create should succeed");

    byte *promoted = Allocator.promote(p, 48, arena);
    Assert.isNotNull(promoted, "promote to arena should succeed");

    Allocator.Resource.release(rs);  // p now invalid

    bool ok = true;
    for (int i = 0; i < 48; i++) {
        if (promoted[i] != (byte)(0x55 ^ i)) { ok = false; break; }
    }
    Assert.isTrue(ok, "promoted data must be intact in arena after rscope release");
    printf("  promoted@%p in arena slot=%zu data intact=%s\n",
           (void *)promoted, arena->scope_id, ok ? "yes" : "no");

    Allocator.Arena.dispose(arena);  // promoted reclaimed with arena
}

// ============================================================================
// PM-05: NULL ptr returns NULL
// ============================================================================
void test_promote_null_ptr_returns_null(void) {
    scope slb0 = Memory.get_scope(1);
    object result = Allocator.promote(NULL, 32, slb0);
    Assert.isNull(result, "promote(NULL, size, dst) must return NULL");
    printf("  promote(NULL, 32, slb0) = %p\n", result);
}

// ============================================================================
// PM-06: size=0 returns NULL
// ============================================================================
void test_promote_zero_size_returns_null(void) {
    scope slb0 = Memory.get_scope(1);
    byte dummy = 42;
    object result = Allocator.promote(&dummy, 0, slb0);
    Assert.isNull(result, "promote(ptr, 0, dst) must return NULL");
    printf("  promote(&dummy, 0, slb0) = %p\n", result);
}

// ============================================================================
// PM-07: NULL dst returns NULL
// ============================================================================
void test_promote_null_dst_returns_null(void) {
    byte dummy = 42;
    object result = Allocator.promote(&dummy, 1, NULL);
    Assert.isNull(result, "promote(ptr, size, NULL) must return NULL");
    printf("  promote(&dummy, 1, NULL) = %p\n", result);
}

// ============================================================================
// PM-08: 256-byte data integrity across rscope → SLB0
// ============================================================================
void test_promote_data_integrity_large(void) {
    rscope rs = Allocator.Resource.acquire(8192);
    Assert.isNotNull(rs, "rscope acquire should succeed");

    byte *p = Allocator.Resource.alloc(rs, 256);
    Assert.isNotNull(p, "rscope alloc 256 should succeed");
    for (int i = 0; i < 256; i++) p[i] = (byte)(i ^ 0xC3);

    scope slb0 = Memory.get_scope(1);
    byte *promoted = Allocator.promote(p, 256, slb0);
    Assert.isNotNull(promoted, "promote 256 bytes to SLB0 should succeed");

    Allocator.Resource.release(rs);  // p now invalid

    bool ok = true;
    for (int i = 0; i < 256; i++) {
        if (promoted[i] != (byte)(i ^ 0xC3)) { ok = false; break; }
    }
    Assert.isTrue(ok, "all 256 promoted bytes must match the original pattern");
    printf("  256-byte promote integrity=%s\n", ok ? "pass" : "FAIL");

    Allocator.dispose(promoted);
}

// ============================================================================
// PM-09: promote into an exhausted rscope returns NULL
// ============================================================================
void test_promote_dst_exhausted_returns_null(void) {
    rscope rs = Allocator.Resource.acquire(16);  // exactly one kAlign slot
    Assert.isNotNull(rs, "small rscope acquire should succeed");

    byte *first = Allocator.Resource.alloc(rs, 16);
    Assert.isNotNull(first, "first alloc should fill slab");

    byte src[16] = {0};
    object result = Allocator.promote(src, 16, (scope)rs);
    Assert.isNull(result, "promote into exhausted rscope must return NULL");
    printf("  promote to exhausted rscope: result=%p (expected NULL)\n", result);

    Allocator.Resource.release(rs);
}
#endif

#if 1  // Region: Test Registration
__attribute__((constructor)) void init_promote_tests(void)
{
    testset("Scope: promote (FT-14)", set_config, set_teardown);

    testcase("PM-01: promote arena frame → SLB0",
             test_promote_frame_to_slb0);
    testcase("PM-02: promote arena frame → rscope",
             test_promote_frame_to_rscope);
    testcase("PM-03: promote rscope → SLB0",
             test_promote_rscope_to_slb0);
    testcase("PM-04: promote rscope → arena",
             test_promote_rscope_to_arena);
    testcase("PM-05: NULL ptr returns NULL",
             test_promote_null_ptr_returns_null);
    testcase("PM-06: size=0 returns NULL",
             test_promote_zero_size_returns_null);
    testcase("PM-07: NULL dst returns NULL",
             test_promote_null_dst_returns_null);
    testcase("PM-08: 256-byte data integrity",
             test_promote_data_integrity_large);
    testcase("PM-09: promote into exhausted rscope returns NULL",
             test_promote_dst_exhausted_returns_null);
}
#endif
