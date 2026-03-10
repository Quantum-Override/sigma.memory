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
 * File: test_frame_interface.c
 * Description: TDD tests for Frame interface - explicit scope targeting
 */

#include <sigma.test/sigtest.h>
#include <stdio.h>
#include "memory.h"

static void set_config(FILE **log_stream) {
    *log_stream = fopen("logs/test_frame_interface.log", "w");
}

static void set_teardown(void) {
}

#if 1  // Region: FI-01 - Frame.begin() uses R7 (current scope)
void test_fi_01_frame_begin_uses_r7(void) {
    /*
     * Frame.begin() must create a frame on the scope pointed to by R7.
     * Frame.depth() must reflect the new frame.
     * R7 (Scope.current()) must not change as a result.
     */
    scope before = Allocator.Scope.current();
    Assert.isNotNull(before, "FI-01: initial scope must be valid");

    usize depth_before = Allocator.Frame.depth();
    Assert.isTrue(depth_before == 0, "FI-01: frame depth must be 0 before begin");

    frame f = Allocator.Frame.begin();
    Assert.isNotNull(f, "FI-01: Frame.begin() must return a valid handle");

    Assert.isTrue(Allocator.Frame.depth() == 1, "FI-01: depth must be 1 after begin");
    Assert.isTrue(Allocator.Scope.current() == before,
                  "FI-01: R7 must not change after Frame.begin");

    integer rc = Allocator.Frame.end(f);
    Assert.isTrue(rc == OK, "FI-01: Frame.end must succeed");
    Assert.isTrue(Allocator.Frame.depth() == 0, "FI-01: depth must return to 0 after end");
}
#endif

#if 1  // Region: FI-02 - Frame.end() frees allocations; second begin while active returns NULL
void test_fi_02_frame_end_decrements_depth(void) {
    /*
     * Allocations made after Frame.begin() are freed when Frame.end()
     * is called.  Only one frame is allowed per scope at a time: a
     * second begin() while a frame is active must return NULL.
     * After Frame.end(), sequential frames work normally.
     */
    Assert.isTrue(Allocator.Frame.depth() == 0, "FI-02: must start at depth 0");

    frame f1 = Allocator.Frame.begin();
    Assert.isNotNull(f1, "FI-02: first frame must be valid");
    Assert.isTrue(Allocator.Frame.depth() == 1, "FI-02: depth must be 1 after begin");

    object p = Allocator.alloc(128);
    Assert.isNotNull(p, "FI-02: alloc within frame must succeed");

    // Single-frame model: a second begin while f1 is active must fail
    frame f_dup = Allocator.Frame.begin();
    Assert.isNull(f_dup, "FI-02: begin while frame active must return NULL");
    Assert.isTrue(Allocator.Frame.depth() == 1, "FI-02: depth unchanged after null begin");

    integer rc = Allocator.Frame.end(f1);
    Assert.isTrue(rc == OK, "FI-02: Frame.end must succeed");
    Assert.isTrue(Allocator.Frame.depth() == 0, "FI-02: depth must return to 0 after end");

    // Sequential use: new frame must succeed after end
    frame f2 = Allocator.Frame.begin();
    Assert.isNotNull(f2, "FI-02: sequential frame must be valid after end");
    rc = Allocator.Frame.end(f2);
    Assert.isTrue(rc == OK && Allocator.Frame.depth() == 0, "FI-02: second frame end OK");
}
#endif

#if 1  // Region: FI-03 - Frame.begin_in() does NOT change R7
void test_fi_03_frame_begin_in_does_not_change_r7(void) {
    /*
     * Frame.begin_in(scope) starts a frame in the specified scope
     * without touching R7.  This enables a frame on SLB0 while
     * an arena is current (the "janky" pattern).
     */
    scope slb0 = Allocator.Scope.current();
    Assert.isNotNull(slb0, "FI-03: initial scope (SLB0) must be valid");

    scope arena = Allocator.Arena.create("fi03", SCOPE_POLICY_DYNAMIC);
    Assert.isNotNull(arena, "FI-03: arena must be created");
    Assert.isTrue(Allocator.Scope.current() == arena, "FI-03: arena must be current");

    // Start a frame in slb0 while arena is still current (R7 unchanged)
    frame f = Allocator.Frame.begin_in(slb0);
    Assert.isNotNull(f, "FI-03: Frame.begin_in(slb0) must return valid frame");

    // R7 must still point at arena
    Assert.isTrue(Allocator.Scope.current() == arena, "FI-03: R7 must not change after begin_in");

    // Frame depth on explicit slb0 should be 1; on current (arena) still 0
    Assert.isTrue(Allocator.Frame.depth_of(slb0) == 1, "FI-03: slb0 frame depth must be 1");
    Assert.isTrue(Allocator.Frame.depth() == 0, "FI-03: arena frame depth must still be 0");

    integer rc = Allocator.Frame.end_in(slb0, f);
    Assert.isTrue(rc == OK, "FI-03: Frame.end_in must succeed");
    Assert.isTrue(Allocator.Frame.depth_of(slb0) == 0, "FI-03: slb0 depth must be 0 after end_in");
    Assert.isTrue(Allocator.Scope.current() == arena, "FI-03: R7 must still be arena after end_in");

    Allocator.Arena.dispose(arena);
}
#endif

#if 1  // Region: FI-04 - Frame.end_in() does NOT change R7
void test_fi_04_frame_end_in_does_not_change_r7(void) {
    /*
     * Frame.end_in(scope, frame) must close the frame in the
     * specified scope without affecting R7.  Verifies that the
     * two-arg explicit form is symmetric with begin_in.
     */
    scope slb0 = Allocator.Scope.current();

    scope arena = Allocator.Arena.create("fi04", SCOPE_POLICY_DYNAMIC);
    Assert.isNotNull(arena, "FI-04: arena must be created");

    frame f = Allocator.Frame.begin_in(slb0);
    Assert.isNotNull(f, "FI-04: frame in slb0 must be valid");

    // Alloc some memory inside the frame on slb0
    object p = Allocator.Scope.alloc(slb0, 256);
    Assert.isNotNull(p, "FI-04: alloc in slb0 must succeed");

    scope before_end = Allocator.Scope.current();

    integer rc = Allocator.Frame.end_in(slb0, f);
    Assert.isTrue(rc == OK, "FI-04: end_in must return OK");

    Assert.isTrue(Allocator.Scope.current() == before_end, "FI-04: R7 unchanged by end_in");
    Assert.isTrue(Allocator.Frame.depth_of(slb0) == 0, "FI-04: slb0 frame depth must be 0");

    Allocator.Arena.dispose(arena);
}
#endif

#if 1  // Region: FI-05 - Frame.depth() reports active frame; second begin returns NULL
void test_fi_05_frame_depth_tracks_nesting(void) {
    /*
     * Frame.depth() reports whether the current scope (R7) has an
     * active frame (0 or 1).  Creating frames on a different scope
     * must not affect the reported depth of the current scope.
     * Attempting a second Frame.begin() while one is active returns NULL.
     */
    scope slb0 = Allocator.Scope.current();
    scope arena = Allocator.Arena.create("fi05", SCOPE_POLICY_DYNAMIC);

    // arena is now current; depth on arena starts at 0
    Assert.isTrue(Allocator.Frame.depth() == 0, "FI-05: arena frame depth starts at 0");

    frame f1 = Allocator.Frame.begin();  // frame in arena
    Assert.isTrue(Allocator.Frame.depth() == 1, "FI-05: arena depth == 1 after begin");

    // Frame in slb0 must NOT affect arena depth
    frame slb0_f = Allocator.Frame.begin_in(slb0);
    Assert.isNotNull(slb0_f, "FI-05: slb0 frame must be valid");
    Assert.isTrue(Allocator.Frame.depth() == 1, "FI-05: arena depth still 1 after slb0 begin_in");

    // Second begin in arena must return NULL (single-frame model)
    frame f2 = Allocator.Frame.begin();
    Assert.isNull(f2, "FI-05: second begin in arena must return NULL");
    Assert.isTrue(Allocator.Frame.depth() == 1, "FI-05: depth unchanged after null begin");

    Allocator.Frame.end_in(slb0, slb0_f);
    Assert.isTrue(Allocator.Frame.depth() == 1, "FI-05: arena depth unchanged by slb0 end_in");

    Allocator.Frame.end(f1);
    Assert.isTrue(Allocator.Frame.depth() == 0, "FI-05: arena depth back to 0");

    Allocator.Arena.dispose(arena);
}
#endif

#if 1  // Region: FI-06 - Frame.depth_of() caps at 1; second begin returns NULL
void test_fi_06_frame_depth_of_queries_specific_scope(void) {
    /*
     * Frame.depth_of(scope) returns 0 or 1 for the named scope.
     * A second Frame.begin() while a frame is active returns NULL and
     * leaves depth_of() unchanged at 1.
     */
    scope slb0 = Allocator.Scope.current();
    scope arena = Allocator.Arena.create("fi06", SCOPE_POLICY_DYNAMIC);
    Assert.isNotNull(arena, "FI-06: arena must be created");

    Assert.isTrue(Allocator.Frame.depth_of(slb0)  == 0, "FI-06: slb0 depth starts at 0");
    Assert.isTrue(Allocator.Frame.depth_of(arena) == 0, "FI-06: arena depth starts at 0");

    frame fa = Allocator.Frame.begin();         // frame in arena (current)
    frame fb = Allocator.Frame.begin_in(slb0);  // frame in slb0 (explicit)

    Assert.isTrue(Allocator.Frame.depth_of(arena) == 1, "FI-06: arena depth == 1");
    Assert.isTrue(Allocator.Frame.depth_of(slb0)  == 1, "FI-06: slb0 depth == 1");

    // Second begin in arena must return NULL; depths must not change
    frame fa2 = Allocator.Frame.begin();
    Assert.isNull(fa2, "FI-06: second begin in arena must be NULL");
    Assert.isTrue(Allocator.Frame.depth_of(arena) == 1, "FI-06: arena depth still 1 after null begin");
    Assert.isTrue(Allocator.Frame.depth_of(slb0)  == 1, "FI-06: slb0 depth unchanged");

    Allocator.Frame.end(fa);
    Allocator.Frame.end_in(slb0, fb);

    Assert.isTrue(Allocator.Frame.depth_of(arena) == 0, "FI-06: arena depth back to 0");
    Assert.isTrue(Allocator.Frame.depth_of(slb0)  == 0, "FI-06: slb0 depth back to 0");

    Allocator.Arena.dispose(arena);
}
#endif

__attribute__((constructor)) void init_test(void) {
    testset("Frame Interface: Explicit Scope Targeting", set_config, set_teardown);
    testcase("FI-01: Frame.begin() uses R7", test_fi_01_frame_begin_uses_r7);
    testcase("FI-02: Frame.end() decrements depth", test_fi_02_frame_end_decrements_depth);
    testcase("FI-03: Frame.begin_in() does not change R7",
             test_fi_03_frame_begin_in_does_not_change_r7);
    testcase("FI-04: Frame.end_in() does not change R7",
             test_fi_04_frame_end_in_does_not_change_r7);
    testcase("FI-05: Frame.depth() tracks nesting", test_fi_05_frame_depth_tracks_nesting);
    testcase("FI-06: Frame.depth_of() queries specific scope",
             test_fi_06_frame_depth_of_queries_specific_scope);
}
