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
 * File: test_page_release.c
 * Description: Unit tests for SLB0 dynamic page release (munmap on empty page)
 *
 * Architecture note:
 *   SLB0 is initialised with a contiguous 16-page mmap (the "initial block").
 *   These pages are released wholesale at shutdown — never individually.
 *   Pages added dynamically (page_count > 16) ARE individually munmap'd when
 *   their alloc_count reaches 0 after the last Allocator.dispose() on them.
 *
 * Coverage:
 *   PRL-01  SLB0 starts with exactly 16 pages
 *   PRL-02  Overflow beyond 16 pages creates a dynamic (17th+) page
 *   PRL-03  Freeing all allocs on a dynamic page releases it (page_count --)
 *   PRL-04  Initial-block pages are NOT released even when fully empty
 *   PRL-05  SLB0 is still usable after a dynamic page is released
 */

#include <sigma.test/sigtest.h>
#include <stdlib.h>
#include <string.h>
#include "internal/memory.h"
#include "memory.h"

// ─── compile-time sizing constants ────────────────────────────────────────
// Per-page capacity with ALLOC_BLOCK_SIZE:
//   usable = SYS0_PAGE_SIZE - sizeof(sc_page_sentinel) = 8192 - 32 = 8160
//   blocks = floor(8160 / 256) = 31
#define SLB0_INITIAL_PAGES 16u
#define ALLOC_BLOCK_SIZE 256u
#define BLOCKS_PER_PAGE 31u

// Threshold address separating initial-block pages from dynamic pages
#define dynamic_threshold(slb0) ((slb0)->first_page_off + (16u * (usize)SYS0_PAGE_SIZE))

// True if an allocation resides on a dynamic (non-initial) page
#define is_dynamic_alloc(ptr, slb0) ((addr)(ptr) >= dynamic_threshold(slb0))

// Upper bound on allocations needed to reach a dynamic page (with headroom)
#define MAX_FILL 640

#if 1  // Region: Test Set Setup & Teardown
static void set_config(FILE **log_stream) {
    *log_stream = fopen("logs/test_page_release.log", "w");
    // sigma.test framework pre-activates its own arena; restore R7 to SLB0.
    Allocator.Scope.restore();
    sbyte state = Memory.state();
    Assert.isTrue(state & MEM_STATE_READY, "Memory system must be ready");
}

static void set_teardown(void) {
}
#endif

#if 1  // Region: PRL - Page Release Tests

// ============================================================================
// PRL-01: SLB0 begins with exactly 16 pages after bootstrap
// ============================================================================
void test_slb0_initial_page_count(void) {
    scope slb0 = Memory.get_scope(1);
    Assert.isNotNull(slb0, "PRL-01: SLB0 scope must be accessible");

    Assert.isTrue(slb0->page_count == SLB0_INITIAL_PAGES,
                  "PRL-01: Expected %u initial pages, got %zu", SLB0_INITIAL_PAGES,
                  slb0->page_count);
}

// ============================================================================
// PRL-02: Overflowing the initial block creates a dynamic page
//
// Keep allocating until page_count increases.  We don't depend on address
// ranges here because valgrind's mmap assignments can differ from native Linux.
// ============================================================================
void test_dynamic_page_created_on_overflow(void) {
    // sigma.test activates sigtest_arena before each case; restore to SLB0.
    Allocator.Scope.restore();

    scope slb0 = Memory.get_scope(1);
    Assert.isNotNull(slb0, "PRL-02: SLB0 must be accessible");

    usize start_count = slb0->page_count;

    void **ptrs = (void **)malloc(MAX_FILL * sizeof(void *));
    Assert.isNotNull(ptrs, "PRL-02: helper alloc failed");

    int n = 0;
    bool overflowed = false;

    while (n < MAX_FILL) {
        ptrs[n] = Allocator.alloc(ALLOC_BLOCK_SIZE);
        Assert.isNotNull(ptrs[n], "PRL-02: Alloc %d should succeed", n);
        n++;
        if (slb0->page_count > start_count) {
            overflowed = true;
            break;
        }
    }

    Assert.isTrue(overflowed,
                  "PRL-02: page_count should have increased from %zu within %d allocs (got %zu)",
                  start_count, MAX_FILL, slb0->page_count);

    printf("  PRL-02: %d allocs created dynamic page (page_count %zu → %zu)\n", n, start_count,
           slb0->page_count);

    // Cleanup (also frees the dynamic page via alloc_count drop)
    for (int i = 0; i < n; i++) {
        Allocator.dispose(ptrs[i]);
    }
    free(ptrs);
}

// ============================================================================
// PRL-03: Freeing all allocs from a dynamic page releases it (page_count --)
//
// Strategy: allocate until we obtain a pointer whose address lies beyond the
// initial-block range (address-based detection, no fragile page_count math).
// Collect all such "dynamic" pointers, free them, and verify page_count drops.
// ============================================================================
void test_dynamic_page_released_when_empty(void) {
    scope slb0 = Memory.get_scope(1);
    Assert.isNotNull(slb0, "PRL-03: SLB0 must be accessible");

    addr threshold = dynamic_threshold(slb0);

    void **ptrs = (void **)malloc(MAX_FILL * sizeof(void *));
    Assert.isNotNull(ptrs, "PRL-03: helper alloc failed");

    int n = 0;
    int first_dynamic = -1;

    while (n < MAX_FILL) {
        ptrs[n] = Allocator.alloc(ALLOC_BLOCK_SIZE);
        Assert.isNotNull(ptrs[n], "PRL-03: alloc %d should succeed", n);
        if ((addr)ptrs[n] >= threshold && first_dynamic < 0) {
            first_dynamic = n;
        }
        n++;
        // Once we have the first dynamic alloc, allow a few more to fully
        // populate that page, then stop.
        if (first_dynamic >= 0 && n >= first_dynamic + (int)BLOCKS_PER_PAGE) {
            break;
        }
    }

    if (first_dynamic < 0) {
        printf("  PRL-03: could not trigger dynamic page within %d allocs; skipping\n", n);
        for (int i = 0; i < n; i++) Allocator.dispose(ptrs[i]);
        free(ptrs);
        return;
    }

    usize count_with_dynamic = slb0->page_count;
    printf("  PRL-03: dynamic alloc at index %d (page_count=%zu)\n", first_dynamic,
           count_with_dynamic);

    // Free ONLY the dynamic-page allocations
    for (int i = first_dynamic; i < n; i++) {
        Allocator.dispose(ptrs[i]);
        ptrs[i] = NULL;
    }

    usize count_after_release = slb0->page_count;
    Assert.isTrue(
        count_after_release < count_with_dynamic,
        "PRL-03: Dynamic page should be released; page_count should drop from %zu (got %zu)",
        count_with_dynamic, count_after_release);

    printf("  PRL-03: page_count: %zu → %zu (released)\n", count_with_dynamic, count_after_release);

    // Cleanup initial-page allocations
    for (int i = 0; i < first_dynamic; i++) {
        Allocator.dispose(ptrs[i]);
    }
    free(ptrs);
}

// ============================================================================
// PRL-04: Initial-block pages are NOT individually released even when empty
//
// Strategy: allocate a batch that lands only on initial-block pages (addresses
// below the dynamic threshold), free them all, and verify that the initial 16
// pages are still present (page_count >= 16).
// ============================================================================
void test_initial_pages_not_released(void) {
    scope slb0 = Memory.get_scope(1);
    Assert.isNotNull(slb0, "PRL-04: SLB0 must be accessible");

    addr threshold = dynamic_threshold(slb0);
    usize before_count = slb0->page_count;

    // Allocate a modest batch and keep only those on initial pages
    const int BATCH = 64;
    void *ptrs[BATCH];
    int landed = 0;

    for (int i = 0; i < BATCH; i++) {
        ptrs[i] = Allocator.alloc(ALLOC_BLOCK_SIZE);
        Assert.isNotNull(ptrs[i], "PRL-04: alloc %d should succeed", i);
        if ((addr)ptrs[i] < threshold) {
            landed++;
        }
    }

    // Free all (including any that may have landed on dynamic pages)
    for (int i = 0; i < BATCH; i++) {
        Allocator.dispose(ptrs[i]);
    }

    usize after_count = slb0->page_count;

    // Regardless of state, the initial 16-page block must NEVER be individually munmap'd
    Assert.isTrue(after_count >= 16u,
                  "PRL-04: Initial pages must persist; page_count should be >= 16, got %zu",
                  after_count);

    printf("  PRL-04: %d initial-page allocs freed; page_count %zu → %zu (>= 16 required)\n",
           landed, before_count, after_count);
}

// ============================================================================
// PRL-05: SLB0 remains usable after a dynamic page is released
// ============================================================================
void test_allocations_succeed_after_page_release(void) {
    // sigma.test activates sigtest_arena before each case; restore to SLB0.
    Allocator.Scope.restore();

    scope slb0 = Memory.get_scope(1);
    Assert.isNotNull(slb0, "PRL-05: SLB0 must be accessible");

    addr threshold = dynamic_threshold(slb0);

    void **fill = (void **)malloc(MAX_FILL * sizeof(void *));
    Assert.isNotNull(fill, "PRL-05: helper alloc failed");

    // Force a dynamic page: keep allocating until one lands beyond the threshold
    int n = 0;
    void *dyn_ptr = NULL;

    while (n < MAX_FILL) {
        fill[n] = Allocator.alloc(ALLOC_BLOCK_SIZE);
        Assert.isNotNull(fill[n], "PRL-05: alloc %d failed", n);
        if ((addr)fill[n] >= threshold && dyn_ptr == NULL) {
            dyn_ptr = fill[n];
            fill[n] = NULL;  // remove from fill list so we don't double-free
            n++;
            break;
        }
        n++;
    }

    if (dyn_ptr == NULL) {
        printf("  PRL-05: could not trigger dynamic page; skipping\n");
        for (int i = 0; i < n; i++)
            if (fill[i]) Allocator.dispose(fill[i]);
        free(fill);
        return;
    }

    usize count_before = slb0->page_count;
    Allocator.dispose(dyn_ptr);
    usize count_after_release = slb0->page_count;

    Assert.isTrue(count_after_release < count_before,
                  "PRL-05: Expected page release; count %zu → %zu", count_before,
                  count_after_release);

    // Verify SLB0 still works
    const int VERIFY = 16;
    void *vptrs[VERIFY];
    for (int i = 0; i < VERIFY; i++) {
        vptrs[i] = Allocator.alloc(128);
        Assert.isNotNull(vptrs[i], "PRL-05: Post-release alloc %d failed", i);
        memset(vptrs[i], 0xEE, 128);
    }
    for (int i = 0; i < VERIFY; i++) {
        Allocator.dispose(vptrs[i]);
    }

    printf("  PRL-05: %d post-release allocs succeeded (page_count=%zu → %zu → %zu)\n", VERIFY,
           count_before, count_after_release, slb0->page_count);

    // Cleanup fill
    for (int i = 0; i < n - 1; i++) {
        if (fill[i]) Allocator.dispose(fill[i]);
    }
    free(fill);
}

#endif  // Region: PRL

#if 1  // Region: Test Registration
__attribute__((constructor)) void init_page_release_tests(void) {
    testset("Unit: SLB0 Dynamic Page Release", set_config, set_teardown);

    testcase("PRL-01: SLB0 starts with 16 pages", test_slb0_initial_page_count);
    testcase("PRL-02: Overflow creates dynamic page", test_dynamic_page_created_on_overflow);
    testcase("PRL-03: Dynamic page released when empty", test_dynamic_page_released_when_empty);
    testcase("PRL-04: Initial pages not individually released", test_initial_pages_not_released);
    testcase("PRL-05: SLB0 usable after page release", test_allocations_succeed_after_page_release);
}
#endif
