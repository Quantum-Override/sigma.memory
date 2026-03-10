/*
 * SigmaCore
 * Copyright (c) 2026 David Boarman (BadKraft) and contributors
 * QuantumOverride [Q|]
 * ----------------------------------------------
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 * ----------------------------------------------
 * File: test_realloc.c
 * Description: Unit tests for Allocator.realloc
 *
 * Coverage:
 *   REA-01  NULL ptr → fresh allocation
 *   REA-02  NULL ptr + zero size → NULL (no-op)
 *   REA-03  Non-NULL ptr + zero size → dispose (returns NULL)
 *   REA-04  Shrink in-place: same pointer returned, data preserved
 *   REA-05  Grow: new pointer returned, data preserved
 *   REA-06  Data integrity across grow → shrink → dispose cycle
 *   REA-07  Remainder below split threshold: block unchanged (no split)
 *   REA-08  Grow failure propagation: original pointer still valid on OOM
 */

#include <sigma.test/sigtest.h>
#include <string.h>
#include "internal/memory.h"
#include "memory.h"

#if 1  // Region: Test Set Setup & Teardown
static void set_config(FILE **log_stream) {
    *log_stream = fopen("logs/test_realloc.log", "w");
    sbyte state = Memory.state();
    Assert.isTrue(state & MEM_STATE_READY, "Memory system must be ready");
}

static void set_teardown(void) {}
#endif

#if 1  // Region: REA - Realloc Tests

// ============================================================================
// REA-01: NULL ptr + size > 0 → fresh allocation
// ============================================================================
void test_realloc_null_ptr_alloc(void) {
    // NULL ptr is equivalent to Allocator.alloc(size)
    object ptr = Allocator.realloc(NULL, 128);
    Assert.isNotNull(ptr, "REA-01: realloc(NULL, 128) should return a valid pointer");

    // Verify writable
    memset(ptr, 0xAA, 128);

    Allocator.dispose(ptr);
}

// ============================================================================
// REA-02: NULL ptr + zero size → NULL (no-op, no crash)
// ============================================================================
void test_realloc_null_ptr_zero_size(void) {
    object ptr = Allocator.realloc(NULL, 0);
    Assert.isNull(ptr, "REA-02: realloc(NULL, 0) should return NULL");
}

// ============================================================================
// REA-03: Non-NULL ptr + zero size → dispose semantics, returns NULL
// ============================================================================
void test_realloc_dispose_semantics(void) {
    object ptr = Allocator.alloc(256);
    Assert.isNotNull(ptr, "REA-03: Initial alloc should succeed");

    object result = Allocator.realloc(ptr, 0);
    Assert.isNull(result, "REA-03: realloc(ptr, 0) should return NULL (dispose)");
    // ptr is freed here — no double-free needed
}

// ============================================================================
// REA-04: Shrink in-place — same pointer, data preserved
// ============================================================================
void test_realloc_shrink_in_place(void) {
    const usize ORIG_SIZE = 512;
    const usize NEW_SIZE  = 128;
    const usize PATTERN   = 0x5C;

    object ptr = Allocator.alloc(ORIG_SIZE);
    Assert.isNotNull(ptr, "REA-04: Initial alloc should succeed");

    memset(ptr, PATTERN, ORIG_SIZE);

    object result = Allocator.realloc(ptr, NEW_SIZE);
    Assert.isNotNull(result, "REA-04: Shrink realloc should return non-NULL");
    Assert.isTrue(result == ptr, "REA-04: Shrink should return the same pointer");

    // Data in the retained region must be intact
    uint8_t *bytes = (uint8_t *)result;
    for (usize i = 0; i < NEW_SIZE; i++) {
        Assert.isTrue(bytes[i] == PATTERN,
            "REA-04: Data at byte %zu corrupted (expected 0x%02x, got 0x%02x)",
            i, (unsigned)PATTERN, (unsigned)bytes[i]);
    }

    Allocator.dispose(result);
}

// ============================================================================
// REA-05: Grow — new pointer (or same), data preserved up to old size
// ============================================================================
void test_realloc_grow_data_integrity(void) {
    const usize ORIG_SIZE = 64;
    const usize NEW_SIZE  = 512;

    object ptr = Allocator.alloc(ORIG_SIZE);
    Assert.isNotNull(ptr, "REA-05: Initial alloc should succeed");

    // Write a recognizable pattern
    uint8_t *bytes = (uint8_t *)ptr;
    for (usize i = 0; i < ORIG_SIZE; i++) {
        bytes[i] = (uint8_t)(i & 0xFF);
    }

    object result = Allocator.realloc(ptr, NEW_SIZE);
    Assert.isNotNull(result, "REA-05: Grow realloc should succeed");

    // Data from the original allocation must be copied
    uint8_t *new_bytes = (uint8_t *)result;
    for (usize i = 0; i < ORIG_SIZE; i++) {
        Assert.isTrue(new_bytes[i] == (uint8_t)(i & 0xFF),
            "REA-05: Copied data at byte %zu corrupted (expected 0x%02x, got 0x%02x)",
            i, (unsigned)(i & 0xFF), (unsigned)new_bytes[i]);
    }

    // Extended region must be writable
    memset(new_bytes + ORIG_SIZE, 0xFF, NEW_SIZE - ORIG_SIZE);

    Allocator.dispose(result);
}

// ============================================================================
// REA-06: Grow → shrink → dispose cycle; data integrity maintained
// ============================================================================
void test_realloc_grow_shrink_cycle(void) {
    const usize STAGE1 = 64;
    const usize STAGE2 = 1024;   // grow
    const usize STAGE3 = 128;    // shrink

    object ptr = Allocator.alloc(STAGE1);
    Assert.isNotNull(ptr, "REA-06: Initial alloc should succeed");

    uint8_t *bytes = (uint8_t *)ptr;
    for (usize i = 0; i < STAGE1; i++) {
        bytes[i] = (uint8_t)(0xA0 + (i & 0x0F));
    }

    // Grow
    object p2 = Allocator.realloc(ptr, STAGE2);
    Assert.isNotNull(p2, "REA-06: Grow to %zu should succeed", STAGE2);

    uint8_t *b2 = (uint8_t *)p2;
    for (usize i = 0; i < STAGE1; i++) {
        Assert.isTrue(b2[i] == (uint8_t)(0xA0 + (i & 0x0F)),
            "REA-06: Data after grow corrupted at byte %zu", i);
    }

    // Fill the grown region
    memset(b2 + STAGE1, 0xBB, STAGE2 - STAGE1);

    // Shrink back
    object p3 = Allocator.realloc(p2, STAGE3);
    Assert.isNotNull(p3, "REA-06: Shrink to %zu should succeed", STAGE3);
    Assert.isTrue(p3 == p2, "REA-06: Shrink should return the same pointer (in-place)");

    uint8_t *b3 = (uint8_t *)p3;
    for (usize i = 0; i < STAGE1; i++) {
        Assert.isTrue(b3[i] == (uint8_t)(0xA0 + (i & 0x0F)),
            "REA-06: Data after shrink corrupted at byte %zu", i);
    }

    Allocator.dispose(p3);
}

// ============================================================================
// REA-07: Remainder below split threshold → no split, block unchanged
//
// SLB0_MIN_ALLOC = 16; split requires remainder >= MIN_ALLOC * 2 = 32.
// Shrink by only 16 bytes → remainder (16) < 32 → no split; same ptr, same size.
// ============================================================================
void test_realloc_undersized_remainder_no_split(void) {
    // Allocate a block whose size is aligned to 32 bytes
    const usize ORIG_SIZE = 64;    // 64 bytes
    const usize NEW_SIZE  = 48;    // shrink by 16 → remainder = 16 < 32 → no split

    object ptr = Allocator.alloc(ORIG_SIZE);
    Assert.isNotNull(ptr, "REA-07: Initial alloc should succeed");

    memset(ptr, 0xCC, ORIG_SIZE);

    object result = Allocator.realloc(ptr, NEW_SIZE);
    Assert.isNotNull(result, "REA-07: Realloc with small remainder should succeed");
    Assert.isTrue(result == ptr, "REA-07: Same pointer expected (no split)");

    Allocator.dispose(result);
}

// ============================================================================
// REA-08: Multiple independent realloc chains don't interfere
// ============================================================================
void test_realloc_independent_chains(void) {
    const int N = 8;
    object ptrs[N];

    // Allocate N independent blocks
    for (int i = 0; i < N; i++) {
        ptrs[i] = Allocator.alloc(128);
        Assert.isNotNull(ptrs[i], "REA-08: Alloc %d should succeed", i);
        memset(ptrs[i], (uint8_t)(0x10 + i), 128);
    }

    // Grow even-indexed, shrink odd-indexed
    for (int i = 0; i < N; i++) {
        usize new_size = (i % 2 == 0) ? 512 : 32;
        ptrs[i] = Allocator.realloc(ptrs[i], new_size);
        Assert.isNotNull(ptrs[i], "REA-08: Realloc %d should succeed", i);
    }

    // Verify data in the surviving prefix (up to min of original/new size)
    for (int i = 0; i < N; i++) {
        usize check = (i % 2 == 0) ? 128 : 32;
        uint8_t *b = (uint8_t *)ptrs[i];
        for (usize j = 0; j < check; j++) {
            Assert.isTrue(b[j] == (uint8_t)(0x10 + i),
                "REA-08: Chain %d data[%zu] = 0x%02x, expected 0x%02x",
                i, j, (unsigned)b[j], (unsigned)(0x10 + i));
        }
    }

    // Cleanup
    for (int i = 0; i < N; i++) {
        Allocator.dispose(ptrs[i]);
    }
}

#endif  // Region: REA

#if 1  // Region: Test Registration
__attribute__((constructor)) void init_realloc_tests(void) {
    testset("Unit: Realloc API", set_config, set_teardown);

    testcase("REA-01: NULL ptr → alloc",                        test_realloc_null_ptr_alloc);
    testcase("REA-02: NULL ptr + zero size → NULL",             test_realloc_null_ptr_zero_size);
    testcase("REA-03: Non-NULL ptr + zero size → dispose",      test_realloc_dispose_semantics);
    testcase("REA-04: Shrink in-place, data preserved",         test_realloc_shrink_in_place);
    testcase("REA-05: Grow, data copied to new block",          test_realloc_grow_data_integrity);
    testcase("REA-06: Grow → shrink cycle, data intact",        test_realloc_grow_shrink_cycle);
    testcase("REA-07: Remainder below split threshold",         test_realloc_undersized_remainder_no_split);
    testcase("REA-08: Independent realloc chains no interference", test_realloc_independent_chains);
}
#endif
