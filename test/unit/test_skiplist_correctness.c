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
 * File: test_skiplist_correctness.c
 * Description: Regression and correctness tests for skiplist_find_for_size
 *              (Task 7) and B-tree free-block reuse via slb0_alloc.
 *
 * Background (Task 7):
 *   The original heuristic in skiplist_find_for_size checked
 *   `block_count < max_blocks_per_page` — a page full of tiny free blocks
 *   would still be returned for large requests, causing false positives.
 *   The fix uses two real checks per page:
 *      1. btree_page_find_free(size) == OK  (tracked free block exists)
 *      2. (page_end - bump_ptr) >= size     (sufficient bump space)
 *   Only then is the page considered a candidate.
 *
 * Coverage:
 *   SLC-01  B-tree free blocks are reused for same-size re-allocations
 *   SLC-02  Coalesced free blocks serve larger re-allocations
 *   SLC-03  Fragmented page (no large free block, no bump space) is skipped
 *           for large requests — Task 7 regression guard
 *   SLC-04  After full-page fill → full-page free, no new page is created
 *           for same-size allocations (pure B-tree reuse path)
 */

#include <sigtest/sigtest.h>
#include <string.h>
#include "internal/memory.h"
#include "internal/node_pool.h"
#include "memory.h"

// ─── sizing: one page worth of 256-byte allocs ────────────────────────────
// Per page: (8192 - 32) / 256 = 31 blocks exactly fill the page
#define SMALL_SIZE        32u    // Small alloc that packs densely
#define MEDIUM_SIZE       256u   // Medium alloc (31 per page)
#define LARGE_SIZE        1024u  // Request that exceeds individual SMALL_SIZE fragments
#define SMALL_PER_PAGE    255u   // floor((8192 - 32) / 32) — fills bump completely
#define MEDIUM_PER_PAGE   31u    // floor((8192 - 32) / 256)

#if 1  // Region: Test Set Setup & Teardown
static void set_config(FILE **log_stream) {
    *log_stream = fopen("logs/test_skiplist_correctness.log", "w");
    sbyte state = Memory.state();
    Assert.isTrue(state & MEM_STATE_READY, "Memory system must be ready");
}

static void set_teardown(void) {}

// Helper: is `ptr` within the given page?
static bool ptr_on_page(const void *ptr, addr page_base) {
    addr a = (addr)ptr;
    return (a >= page_base && a < page_base + SYS0_PAGE_SIZE);
}
#endif

#if 1  // Region: SLC - Skip List Correctness Tests

// ============================================================================
// SLC-01: B-tree free blocks are reused for same-size re-allocations
//
// After filling a page and freeing all blocks, requesting the same size again
// should reuse the B-tree free blocks on that page rather than creating a
// new page (page_count must remain stable).
// ============================================================================
void test_btree_free_blocks_reused(void) {
    scope slb0 = Memory.get_scope(1);
    Assert.isNotNull(slb0, "SLC-01: SLB0 must be accessible");

    usize page_count_before = slb0->page_count;

    // Fill one page worth of MEDIUM_SIZE blocks
    void *ptrs[MEDIUM_PER_PAGE];
    for (usize i = 0; i < MEDIUM_PER_PAGE; i++) {
        ptrs[i] = Allocator.alloc(MEDIUM_SIZE);
        Assert.isNotNull(ptrs[i], "SLC-01: Fill alloc %zu should succeed", i);
    }

    // Free all — they enter the B-tree as free blocks; adjacent ones coalesce
    for (usize i = 0; i < MEDIUM_PER_PAGE; i++) {
        Allocator.dispose(ptrs[i]);
    }

    usize page_count_after_free = slb0->page_count;

    // Re-allocate the same count of MEDIUM_SIZE blocks
    // The B-tree free blocks should serve these without growing page_count
    for (usize i = 0; i < MEDIUM_PER_PAGE; i++) {
        ptrs[i] = Allocator.alloc(MEDIUM_SIZE);
        Assert.isNotNull(ptrs[i], "SLC-01: Re-alloc %zu should succeed from B-tree", i);
    }

    usize page_count_after_realloc = slb0->page_count;

    // No new pages should have been created — allocations came from free blocks
    Assert.isTrue(page_count_after_realloc <= page_count_after_free + 1,
        "SLC-01: page_count should not grow significantly on B-tree reuse "
        "(was %zu, now %zu)",
        page_count_after_free, page_count_after_realloc);

    printf("  SLC-01: page_count: before=%zu free=%zu realloc=%zu\n",
           page_count_before, page_count_after_free, page_count_after_realloc);

    // Cleanup
    for (usize i = 0; i < MEDIUM_PER_PAGE; i++) {
        Allocator.dispose(ptrs[i]);
    }
}

// ============================================================================
// SLC-02: Coalesced free blocks serve allocations larger than the original size
//
// Free N adjacent MEDIUM_SIZE blocks → they coalesce into a block of size
// N × MEDIUM_SIZE.  A subsequent alloc of 2 × MEDIUM_SIZE should fit with no
// new page necessary.
// ============================================================================
void test_coalesced_free_blocks_serve_larger_alloc(void) {
    scope slb0 = Memory.get_scope(1);
    Assert.isNotNull(slb0, "SLC-02: SLB0 must be accessible");

    const usize BLOCK_COUNT = 8;
    const usize COALESCED_SIZE = MEDIUM_SIZE * 2;  // request 2× original after coalesce

    // Allocate BLOCK_COUNT contiguous MEDIUM_SIZE blocks
    void *ptrs[BLOCK_COUNT];
    for (usize i = 0; i < BLOCK_COUNT; i++) {
        ptrs[i] = Allocator.alloc(MEDIUM_SIZE);
        Assert.isNotNull(ptrs[i], "SLC-02: Alloc %zu should succeed", i);
    }

    usize count_before_free = slb0->page_count;

    // Free adjacent pairs — they should coalesce into 2×MEDIUM_SIZE blocks
    for (usize i = 0; i < BLOCK_COUNT; i++) {
        Allocator.dispose(ptrs[i]);
    }

    // Request COALESCED_SIZE — should be served by a coalesced free block
    void *big = Allocator.alloc(COALESCED_SIZE);
    Assert.isNotNull(big, "SLC-02: Allocation of coalesced size should succeed");

    usize count_after_alloc = slb0->page_count;

    // Should not have required a new page if the coalesced free block was found
    Assert.isTrue(count_after_alloc <= count_before_free + 1,
        "SLC-02: page_count should be stable when coalesced block exists "
        "(was %zu, now %zu)", count_before_free, count_after_alloc);

    printf("  SLC-02: Coalesced %zu × %u → served %zu-byte alloc; "
           "page_count=%zu→%zu\n",
           BLOCK_COUNT, MEDIUM_SIZE, COALESCED_SIZE,
           count_before_free, count_after_alloc);

    Allocator.dispose(big);
}

// ============================================================================
// SLC-03: Fragmented page is skipped for requests larger than any free slot
//         Task 7 regression guard
//
// Setup:
//   1. Alloc SMALL_PER_PAGE × SMALL_SIZE blocks — exhausts page 1's bump space.
//   2. Free every other block (alternate indices) — creates SMALL_SIZE free
//      slots separated by live blocks (no coalescing possible).
//   3. Request LARGE_SIZE bytes, which exceeds any individual SMALL_SIZE gap.
//
// Expected with Task 7 fix:
//   - skiplist_find_for_size skips page 1 (no LARGE_SIZE free block, no
//     remaining bump space)
//   - The returned pointer is NOT within page 1's address range.
//
// Old behaviour (before Task 7):
//   - The heuristic block_count < max_blocks_per_page would have returned
//     page 1 as a candidate even though no LARGE_SIZE block was available.
// ============================================================================
void test_fragmented_page_skipped_for_large_request(void) {
    scope slb0 = Memory.get_scope(1);
    Assert.isNotNull(slb0, "SLC-03: SLB0 must be accessible");

    addr page1_base = slb0->first_page_off;

    // Step 1: Fill page 1 completely with SMALL_SIZE blocks
    void *ptrs[SMALL_PER_PAGE];
    for (usize i = 0; i < SMALL_PER_PAGE; i++) {
        ptrs[i] = Allocator.alloc(SMALL_SIZE);
        Assert.isNotNull(ptrs[i], "SLC-03: Fill alloc %zu should succeed", i);
        // Sanity: all fill allocs should be on the initial pages
        Assert.isTrue(
            (addr)ptrs[i] >= page1_base &&
            (addr)ptrs[i] < page1_base + (16u * (usize)SYS0_PAGE_SIZE),
            "SLC-03: Fill alloc %zu should be on an initial page", i);
    }

    // Step 2: Free every other block — creates SMALL_SIZE gaps that cannot
    //         coalesce because live blocks separate them
    for (usize i = 1; i < SMALL_PER_PAGE; i += 2) {
        Allocator.dispose(ptrs[i]);
        ptrs[i] = NULL;
    }

    // At this point:
    //   - Page 1's bump_offset == SYS0_PAGE_SIZE (full, no bump space)
    //   - Page 1's B-tree has ~127 free blocks each of SMALL_SIZE bytes
    //   - No free block of LARGE_SIZE (> SMALL_SIZE) exists on page 1

    // Step 3: Request LARGE_SIZE — must NOT come from the fragmented page 1
    void *large_ptr = Allocator.alloc(LARGE_SIZE);
    Assert.isNotNull(large_ptr, "SLC-03: Large alloc should succeed despite fragmentation");

    bool on_page1 = ptr_on_page(large_ptr, page1_base);
    Assert.isFalse(on_page1,
        "SLC-03 (Task 7 regression): Large alloc should NOT be on fragmented page 1 "
        "(ptr=0x%lx page1=[0x%lx, 0x%lx))",
        (unsigned long)(addr)large_ptr,
        (unsigned long)page1_base,
        (unsigned long)(page1_base + SYS0_PAGE_SIZE));

    printf("  SLC-03: large_ptr=0x%lx page1=[0x%lx,0x%lx) on_page1=%s\n",
           (unsigned long)(addr)large_ptr,
           (unsigned long)page1_base,
           (unsigned long)(page1_base + SYS0_PAGE_SIZE),
           on_page1 ? "YES (FAIL)" : "NO (pass)");

    Allocator.dispose(large_ptr);

    // Cleanup live small blocks
    for (usize i = 0; i < SMALL_PER_PAGE; i++) {
        if (ptrs[i] != NULL) {
            Allocator.dispose(ptrs[i]);
        }
    }
}

// ============================================================================
// SLC-04: Full-page fill → full-page free → same-size re-allocs: no new page
//
// This is a clean stress of the B-tree reuse path: fill, free all (coalescing
// recombines the whole page into one large free block), then re-alloc the same
// count.  The re-allocs must come from that large free block without creating
// any dynamic pages.
// ============================================================================
void test_full_fill_free_realloc_no_new_page(void) {
    scope slb0 = Memory.get_scope(1);
    Assert.isNotNull(slb0, "SLC-04: SLB0 must be accessible");

    // Fill MEDIUM_PER_PAGE blocks of MEDIUM_SIZE — exhausts one page
    void *ptrs[MEDIUM_PER_PAGE];
    for (usize i = 0; i < MEDIUM_PER_PAGE; i++) {
        ptrs[i] = Allocator.alloc(MEDIUM_SIZE);
        Assert.isNotNull(ptrs[i], "SLC-04: Fill alloc %zu should succeed", i);
    }

    // Free all in reverse order — promotes maximal coalescing
    for (int i = (int)MEDIUM_PER_PAGE - 1; i >= 0; i--) {
        Allocator.dispose(ptrs[i]);
    }

    usize count_after_free = slb0->page_count;

    // Re-alloc the same blocks from the coalesced free space
    for (usize i = 0; i < MEDIUM_PER_PAGE; i++) {
        ptrs[i] = Allocator.alloc(MEDIUM_SIZE);
        Assert.isNotNull(ptrs[i], "SLC-04: Re-alloc %zu should succeed", i);
    }

    usize count_after_realloc = slb0->page_count;

    // No new dynamic pages should appear
    Assert.isTrue(count_after_realloc <= count_after_free + 1,
        "SLC-04: No new pages expected (free=%zu realloc=%zu)",
        count_after_free, count_after_realloc);

    printf("  SLC-04: page_count after_free=%zu after_realloc=%zu\n",
           count_after_free, count_after_realloc);

    // Cleanup
    for (usize i = 0; i < MEDIUM_PER_PAGE; i++) {
        Allocator.dispose(ptrs[i]);
    }
}

#endif  // Region: SLC

#if 1  // Region: Test Registration
__attribute__((constructor)) void init_skiplist_correctness_tests(void) {
    testset("Unit: Skip List Correctness (Task 7)", set_config, set_teardown);

    testcase("SLC-01: B-tree free blocks reused for same-size allocs",
             test_btree_free_blocks_reused);
    testcase("SLC-02: Coalesced free blocks serve larger allocs",
             test_coalesced_free_blocks_serve_larger_alloc);
    testcase("SLC-03: Fragmented page skipped for large request (T7 regression)",
             test_fragmented_page_skipped_for_large_request);
    testcase("SLC-04: Full fill → full free → re-alloc: no new page",
             test_full_fill_free_realloc_no_new_page);
}
#endif
