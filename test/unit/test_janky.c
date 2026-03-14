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
 * File: test_janky.c
 * Description: Stress tests for mixed-scope usage (the "janky" patterns)
 */

#include <sigma.test/sigtest.h>
#include <stdio.h>
#include <stdlib.h>
#include "internal/memory.h"
#include "internal/node_pool.h"
#include "memory.h"

static void set_config(FILE **log_stream) {
    *log_stream = fopen("logs/test_janky.log", "w");
    // sigma.test framework pre-activates its own arena; restore R7 to SLB0.
    Allocator.Scope.restore();
}

static void set_teardown(void) {
}

#if 1  // Region: JK-01 - Frame in SLB0 while arena is current
void test_jk_01_frame_in_slb0_while_arena_current(void) {
    /*
     * Cache SLB0, create an arena (which becomes current), then open
     * a frame explicitly in SLB0 using Frame.begin_in().  Allocations
     * via Scope.alloc(slb0, ...) must be tracked by the SLB0 frame,
     * while Allocator.alloc() routes to the arena.
     */
    Allocator.Scope.restore();  // ensure SLB0 is starting scope
    scope slb0 = Allocator.Scope.current();
    Assert.isNotNull(slb0, "JK-01: SLB0 must be valid");

    scope arena = Allocator.Arena.create("jk01", SCOPE_POLICY_DYNAMIC);
    Assert.isNotNull(arena, "JK-01: arena must be created");
    Assert.isTrue(Allocator.Scope.current() == arena, "JK-01: arena must be current");

    // Open a frame in SLB0 (not the current scope)
    frame slb0_frame = Allocator.Frame.begin_in(slb0);
    Assert.isNotNull(slb0_frame, "JK-01: frame in slb0 must be valid");
    Assert.isTrue(Allocator.Scope.current() == arena, "JK-01: R7 unchanged after begin_in");

    // Alloc via arena (implicit R7 path)
    object arena_ptr = Allocator.alloc(128);
    Assert.isNotNull(arena_ptr, "JK-01: arena alloc must succeed");

    // Alloc via explicit slb0 path - should be tracked by slb0_frame
    object slb0_ptr = Allocator.Scope.alloc(slb0, 64);
    Assert.isNotNull(slb0_ptr, "JK-01: slb0 alloc must succeed");

    usize slb0_framed = Allocator.Frame.allocated(slb0_frame);
    Assert.isTrue(slb0_framed >= 64, "JK-01: slb0 frame must track its allocation");

    // End slb0 frame - slb0_ptr freed; arena_ptr survives
    integer rc = Allocator.Frame.end_in(slb0, slb0_frame);
    Assert.isTrue(rc == OK, "JK-01: end_in on slb0 frame must succeed");
    Assert.isTrue(Allocator.Scope.current() == arena, "JK-01: R7 still arena after slb0 end_in");

    // arena_ptr is still valid (different scope)
    Allocator.dispose(arena_ptr);

    Allocator.Arena.dispose(arena);
    Assert.isTrue(Allocator.Scope.current() == slb0, "JK-01: slb0 restored after arena dispose");
}
#endif

#if 1  // Region: JK-02 - alloc() routes to correct scope
void test_jk_02_alloc_routes_to_current_scope(void) {
    /*
     * Allocator.alloc() must route to the scope identified by R7.
     * Scope.alloc(scope, size) must always route to the given scope
     * regardless of R7.  Both paths must coexist without interfering.
     */
    scope slb0 = Allocator.Scope.current();

    scope arena = Allocator.Arena.create("jk02", SCOPE_POLICY_DYNAMIC);
    Assert.isNotNull(arena, "JK-02: arena must be created");

    // Implicit alloc goes to arena (R7)
    object p_arena = Allocator.alloc(256);
    Assert.isNotNull(p_arena, "JK-02: implicit alloc must route to arena");

    // Explicit alloc goes to slb0
    object p_slb0 = Allocator.Scope.alloc(slb0, 256);
    Assert.isNotNull(p_slb0, "JK-02: explicit alloc to slb0 must succeed");

    // Both pointers must be distinct
    Assert.isTrue(p_arena != p_slb0, "JK-02: pointers from different scopes must differ");

    Allocator.dispose(p_arena);
    Allocator.Scope.dispose(slb0, p_slb0);

    Allocator.Arena.dispose(arena);
}
#endif

#if 1  // Region: JK-03 - Arena.dispose auto-unwinds the active frame
void test_jk_03_dispose_arena_auto_unwinds_frames(void) {
    /*
     * If an arena has an active frame when Arena.dispose() is called,
     * the frame must be unwound automatically (no memory leak) and
     * R7 must be restored to the prior scope.
     * A second begin() while a frame is active must return NULL.
     */
    scope slb0 = Allocator.Scope.current();

    scope arena = Allocator.Arena.create("jk03", SCOPE_POLICY_DYNAMIC);
    Assert.isNotNull(arena, "JK-03: arena must be created");

    frame f1 = Allocator.Frame.begin();
    Assert.isNotNull(f1, "JK-03: frame must be valid");
    Assert.isTrue(Allocator.Frame.depth() == 1, "JK-03: depth after begin == 1");

    // Single-frame model: second begin must be rejected
    frame f2 = Allocator.Frame.begin();
    Assert.isNull(f2, "JK-03: second begin must return NULL");
    Assert.isTrue(Allocator.Frame.depth() == 1, "JK-03: depth unchanged after null begin");

    // Allocate inside the frame so dispose has real cleanup to do
    object p1 = Allocator.alloc(256);
    object p2 = Allocator.alloc(512);
    (void)p1;
    (void)p2;

    // Dispose arena with active frame - must auto-unwind
    Allocator.Arena.dispose(arena);

    // R7 must be restored to slb0
    Assert.isTrue(Allocator.Scope.current() == slb0, "JK-03: slb0 restored after arena dispose");

    // Frame ops on slb0 must work normally afterwards
    Assert.isTrue(Allocator.Frame.depth() == 0, "JK-03: slb0 frame depth must be 0");
    frame sf = Allocator.Frame.begin();
    Assert.isNotNull(sf, "JK-03: new frame on slb0 must succeed after cleanup");
    Allocator.Frame.end(sf);
}
#endif

#if 1  // Region: JK-04 - Frame.end_in with invalid handle returns ERR
void test_jk_04_end_in_invalid_handle_returns_err(void) {
    /*
     * Passing a frame handle that does not belong to the specified
     * scope must return ERR.  The system must remain coherent
     * (no crash, no corruption - subsequent operations must work).
     */
    scope slb0 = Allocator.Scope.current();

    scope arena = Allocator.Arena.create("jk04", SCOPE_POLICY_DYNAMIC);
    Assert.isNotNull(arena, "JK-04: arena must be created");

    // Create a frame in arena
    frame arena_frame = Allocator.Frame.begin();
    Assert.isNotNull(arena_frame, "JK-04: arena frame must be valid");

    // Try to end arena's frame via slb0 path - wrong scope
    integer rc = Allocator.Frame.end_in(slb0, arena_frame);
    Assert.isTrue(rc == ERR, "JK-04: end_in with mismatched scope/frame must return ERR");

    // System must still be usable
    Assert.isTrue(Allocator.Frame.depth() == 1, "JK-04: arena frame depth must still be 1");
    Assert.isTrue(Allocator.Scope.current() == arena, "JK-04: R7 must still be arena");

    // Proper cleanup
    rc = Allocator.Frame.end(arena_frame);
    Assert.isTrue(rc == OK, "JK-04: legitimate frame end must succeed after error case");
    Assert.isTrue(Allocator.Frame.depth() == 0, "JK-04: arena frame depth must be 0 after cleanup");

    Allocator.Arena.dispose(arena);
}
#endif

#if 1  // Region: JK-05 - Non-frame allocations survive Frame.end()
void test_jk_05_non_frame_allocs_survive_frame_end(void) {
    /*
     * Allocations made before Frame.begin() are not owned by the frame
     * and must NOT be freed when Frame.end() is called.  Only allocations
     * made after begin() and before end() belong to the frame.
     */
    Allocator.Scope.restore();  // ensure SLB0 is starting scope
    scope slb0 = Allocator.Scope.current();
    Assert.isNotNull(slb0, "JK-05: SLB0 must be valid");

    // Pre-frame allocation - must survive frame end
    object pre_ptr = Allocator.alloc(512);
    Assert.isNotNull(pre_ptr, "JK-05: pre-frame alloc must succeed");

    frame f = Allocator.Frame.begin();
    Assert.isNotNull(f, "JK-05: frame must be valid");

    // In-frame allocation - freed on frame end
    object in_ptr = Allocator.alloc(256);
    Assert.isNotNull(in_ptr, "JK-05: in-frame alloc must succeed");

    usize alloc_bytes = Allocator.Frame.allocated(f);
    Assert.isTrue(alloc_bytes >= 256, "JK-05: frame must track in-frame allocation");

    // End frame - in_ptr freed, pre_ptr untouched
    integer rc = Allocator.Frame.end(f);
    Assert.isTrue(rc == OK, "JK-05: frame end must succeed");

    // pre_ptr must still be accessible; write to trigger fault if freed
    ((char *)pre_ptr)[0] = 0x55;
    Assert.isTrue(((char *)pre_ptr)[0] == 0x55,
                  "JK-05: pre-frame pointer must remain valid after frame end");

    Allocator.dispose(pre_ptr);
    // in_ptr intentionally not disposed - freed by frame
}
#endif

#if 1  // Region: JK-DBG - B-tree tracking reproduction apparatus
/*
 * These tests atomize the exact failure scenario exposed by PRL-05:
 *
 *   slb0_alloc bumps alloc_count on the page sentinel, then calls
 *   btree_page_insert to register the allocation in the page's B-tree.
 *   When btree_page_insert fails silently (e.g. nodepool exhausted or
 *   mremap moved the pool), alloc_count is non-zero but the B-tree has
 *   no entry.  slb0_dispose then cannot find the node, returns early
 *   without decrementing alloc_count, and the page is never released.
 *
 * DBG-01: isolate the tracking contract for a single allocation.
 * DBG-02: reproduce the full PRL-05 scenario (fill → dynamic page →
 *         verify B-tree → dispose → verify page release).
 */

// ============================================================================
// JK-DBG-01: B-tree correctly tracks a single SLB0 allocation
// ============================================================================
void test_jk_dbg_01_btree_tracks_slb0_alloc(void) {
    Allocator.Scope.restore();
    scope slb0 = Memory.get_scope(1);
    Assert.isNotNull(slb0, "DBG-01: SLB0 must be accessible");

    // ── allocate one block ────────────────────────────────────────────────
    object ptr = Allocator.alloc(256);
    Assert.isNotNull(ptr, "DBG-01: alloc must succeed");

    // ── the skip list must know which page owns this pointer ──────────────
    uint16_t page_idx = PAGE_NODE_NULL;
    int skip_rc = skiplist_find_containing(slb0, (addr)ptr, &page_idx);
    Assert.isTrue(skip_rc == OK, "DBG-01: skiplist_find_containing must succeed (rc=%d)", skip_rc);

    // ── the page's B-tree must have a node for this exact address ─────────
    node_idx nidx = NODE_NULL;
    int btree_rc = btree_page_search(slb0, page_idx, (addr)ptr, &nidx);
    Assert.isTrue(btree_rc == OK,
                  "DBG-01: btree_page_search must find allocation on page %u (rc=%d)", page_idx,
                  btree_rc);

    // ── page sentinel must reflect the live allocation ────────────────────
    page_node *pn = nodepool_get_page_node(slb0, page_idx);
    Assert.isNotNull(pn, "DBG-01: page_node must exist");
    Assert.isTrue(pn->alloc_count >= 1, "DBG-01: page alloc_count must be >= 1 (got %u)",
                  pn->alloc_count);

    printf("  DBG-01: page_idx=%u alloc_count=%u B-tree node=%u ptr=%p\n", page_idx,
           pn->alloc_count, nidx, ptr);

    // ── dispose must decrement alloc_count ───────────────────────────────
    uint16_t ac_before = pn->alloc_count;
    Allocator.dispose(ptr);
    Assert.isTrue(pn->alloc_count < ac_before,
                  "DBG-01: alloc_count must decrease after dispose (%u → %u)", ac_before,
                  pn->alloc_count);
}

// ============================================================================
// JK-DBG-02: B-tree tracks the first alloc on a new dynamic page;
//             disposing that alloc releases the page (page_count --)
// ============================================================================
void test_jk_dbg_02_dynamic_page_btree_and_release(void) {
    Allocator.Scope.restore();
    scope slb0 = Memory.get_scope(1);
    Assert.isNotNull(slb0, "DBG-02: SLB0 must be accessible");

    usize start_count = slb0->page_count;

    // ── fill until page_count increases (new dynamic page created) ────────
    const int MAX = 1024;
    void **ptrs = (void **)malloc(MAX * sizeof(void *));
    Assert.isNotNull(ptrs, "DBG-02: helper alloc failed");

    int n = 0;
    void *dyn_ptr = NULL;

    while (n < MAX) {
        ptrs[n] = Allocator.alloc(256);
        Assert.isNotNull(ptrs[n], "DBG-02: alloc %d failed", n);
        if (slb0->page_count > start_count && dyn_ptr == NULL) {
            dyn_ptr = ptrs[n];
            ptrs[n] = NULL;  // don't double-free via cleanup loop
        }
        n++;
        if (dyn_ptr != NULL) break;
    }

    Assert.isNotNull(dyn_ptr, "DBG-02: page_count must increase within %d allocs (remained %zu)",
                     MAX, slb0->page_count);

    printf("  DBG-02: dynamic alloc at n=%d  page_count=%zu  dyn_ptr=%p\n", n - 1, slb0->page_count,
           dyn_ptr);

    // ── the skip list must find the dynamic page ──────────────────────────
    uint16_t page_idx = PAGE_NODE_NULL;
    int skip_rc = skiplist_find_containing(slb0, (addr)dyn_ptr, &page_idx);
    Assert.isTrue(skip_rc == OK, "DBG-02: skiplist must find dynamic page for dyn_ptr (rc=%d)",
                  skip_rc);

    // ── the B-tree must have an entry for dyn_ptr ─────────────────────────
    node_idx nidx = NODE_NULL;
    int btree_rc = btree_page_search(slb0, page_idx, (addr)dyn_ptr, &nidx);
    Assert.isTrue(btree_rc == OK, "DBG-02: B-tree must track dyn_ptr on page %u (rc=%d)", page_idx,
                  btree_rc);

    page_node *pn = nodepool_get_page_node(slb0, page_idx);
    Assert.isNotNull(pn, "DBG-02: page_node must exist");

    printf("  DBG-02: page_idx=%u alloc_count=%u B-tree node=%u\n", page_idx, pn->alloc_count,
           nidx);

    // ── free all fill allocs except dyn_ptr ───────────────────────────────
    for (int i = 0; i < n - 1; i++) {
        if (ptrs[i]) Allocator.dispose(ptrs[i]);
    }
    free(ptrs);

    // ── dispose dyn_ptr — its page must be released (page_count --) ───────
    usize count_before = slb0->page_count;
    Allocator.dispose(dyn_ptr);
    usize count_after = slb0->page_count;

    Assert.isTrue(count_after < count_before,
                  "DBG-02: dynamic page must release after last alloc disposed "
                  "(page_count %zu → %zu)",
                  count_before, count_after);

    printf("  DBG-02: page_count %zu → %zu (dynamic page released)\n", count_before, count_after);
}
#endif  // Region: JK-DBG

__attribute__((constructor)) void init_test(void) {
    testset("Janky: Mixed-Scope Usage", set_config, set_teardown);
    // DBG tests run first — they require a clean SLB0 state (page_count == 16).
    // JK-01..05 may leak dynamic pages if their alloc_count desync is present,
    // so DBG tests must execute before any JK test dirtied the SLB0 state.
    testcase("JK-DBG-01: B-tree tracks a single SLB0 alloc",
             test_jk_dbg_01_btree_tracks_slb0_alloc);
    testcase("JK-DBG-02: B-tree tracks first dynamic-page alloc; dispose releases page",
             test_jk_dbg_02_dynamic_page_btree_and_release);
    testcase("JK-01: frame in SLB0 while arena current",
             test_jk_01_frame_in_slb0_while_arena_current);
    testcase("JK-02: alloc() routes to current scope", test_jk_02_alloc_routes_to_current_scope);
    testcase("JK-03: Arena.dispose auto-unwinds frames",
             test_jk_03_dispose_arena_auto_unwinds_frames);
    testcase("JK-04: end_in with invalid handle returns ERR",
             test_jk_04_end_in_invalid_handle_returns_err);
    testcase("JK-05: non-frame allocs survive frame end",
             test_jk_05_non_frame_allocs_survive_frame_end);
}
