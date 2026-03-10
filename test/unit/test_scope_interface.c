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
 * File: test_scope_interface.c
 * Description: TDD tests for Scope interface R7 stack management
 */

#include <sigma.test/sigtest.h>
#include <stdio.h>
#include "memory.h"

static void set_config(FILE **log_stream) {
    *log_stream = fopen("logs/test_scope_interface.log", "w");
}

static void set_teardown(void) {
}

#if 1  // Region: SI-01 - current() returns SLB0 at startup
void test_si_01_current_is_slb0_at_startup(void) {
    /*
     * After bootstrap, R7 should point at SLB0 (scope_table[1]).
     * Scope.current() must return a non-NULL pointer and be
     * the same scope that Allocator.alloc() uses by default.
     */
    scope s = Allocator.Scope.current();
    Assert.isNotNull(s, "SI-01: Scope.current() should not be NULL at startup");

    // Allocate something through default path and through Scope.alloc with result of current()
    object p1 = Allocator.alloc(64);
    Assert.isNotNull(p1, "SI-01: Default alloc should succeed on SLB0");

    object p2 = Allocator.Scope.alloc(s, 64);
    Assert.isNotNull(p2, "SI-01: Scope.alloc(current()) should succeed");

    Allocator.dispose(p1);
    Allocator.Scope.dispose(s, p2);
}
#endif

#if 1  // Region: SI-02 - Arena.create sets R7 automatically
void test_si_02_create_arena_sets_r7(void) {
    /*
     * Arena.create() should push the old R7 and set R7 to the
     * newly created arena so that Allocator.alloc() immediately
     * targets the new arena without an explicit Scope.set() call.
     */
    scope before = Allocator.Scope.current();
    Assert.isNotNull(before, "SI-02: initial scope must be valid");

    scope arena = Allocator.Arena.create("si02", SCOPE_POLICY_DYNAMIC);
    Assert.isNotNull(arena, "SI-02: Arena.create should return a valid scope");

    scope after = Allocator.Scope.current();
    Assert.isTrue(after == arena, "SI-02: R7 must point to the new arena after Arena.create");
    Assert.isTrue(after != before, "SI-02: R7 must have changed from the previous scope");

    Allocator.Arena.dispose(arena);
}
#endif

#if 1  // Region: SI-03 - Arena.dispose restores R7
void test_si_03_dispose_arena_restores_r7(void) {
    /*
     * Arena.dispose() should pop R7 back to the scope that was
     * current before Arena.create() was called.
     */
    scope before = Allocator.Scope.current();

    scope arena = Allocator.Arena.create("si03", SCOPE_POLICY_DYNAMIC);
    Assert.isNotNull(arena, "SI-03: Arena.create must succeed");
    Assert.isTrue(Allocator.Scope.current() == arena, "SI-03: arena must be current after create");

    Allocator.Arena.dispose(arena);

    scope restored = Allocator.Scope.current();
    Assert.isTrue(restored == before,
                  "SI-03: R7 must be restored to prior scope after Arena.dispose");
}
#endif

#if 1  // Region: SI-04 - Scope.set pushes R7 stack
void test_si_04_scope_set_pushes_r7(void) {
    /*
     * Scope.set(s) should save the current R7 onto the R7 stack
     * and update R7 to s, making restore() possible after.
     */
    scope slb0 = Allocator.Scope.current();
    Assert.isNotNull(slb0, "SI-04: initial scope must be valid");

    scope arena = Allocator.Arena.create("si04", SCOPE_POLICY_DYNAMIC);
    Assert.isNotNull(arena, "SI-04: arena must be created");
    // Arena.create already pushed R7; restore to start clean
    Allocator.Arena.dispose(arena);
    Assert.isTrue(Allocator.Scope.current() == slb0, "SI-04: back to slb0 after dispose");

    // Now create a fresh arena and switch back to slb0 manually
    scope arena2 = Allocator.Arena.create("si04b", SCOPE_POLICY_DYNAMIC);
    Assert.isNotNull(arena2, "SI-04: second arena must be created");
    Assert.isTrue(Allocator.Scope.current() == arena2, "SI-04: arena2 must be current");

    // Explicitly set back to slb0, pushing arena2 ref onto r7 stack
    integer rc = Allocator.Scope.set(slb0);
    Assert.isTrue(rc == OK, "SI-04: Scope.set must return OK");
    Assert.isTrue(Allocator.Scope.current() == slb0, "SI-04: R7 must now be slb0");

    // Restore should go back to arena2
    Allocator.Scope.restore();
    Assert.isTrue(Allocator.Scope.current() == arena2, "SI-04: restore must return to arena2");

    Allocator.Arena.dispose(arena2);
}
#endif

#if 1  // Region: SI-05 - Scope.restore pops R7 stack LIFO
void test_si_05_scope_restore_is_lifo(void) {
    /*
     * Successive Scope.set() calls build a stack; successive
     * Scope.restore() calls unwind it in reverse (LIFO) order.
     */
    scope base = Allocator.Scope.current();

    scope a1 = Allocator.Arena.create("si05a", SCOPE_POLICY_DYNAMIC);
    scope a2 = Allocator.Arena.create("si05b", SCOPE_POLICY_DYNAMIC);
    // Stack now: [base, a1], current = a2

    Assert.isTrue(Allocator.Scope.current() == a2, "SI-05: a2 must be current");

    Allocator.Scope.restore();
    Assert.isTrue(Allocator.Scope.current() == a1, "SI-05: first restore should yield a1");

    Allocator.Scope.restore();
    Assert.isTrue(Allocator.Scope.current() == base, "SI-05: second restore should yield base");

    // Clean up arenas (no longer current so use explicit dispose)
    Allocator.Arena.dispose(a2);
    Allocator.Arena.dispose(a1);
}
#endif

#if 1  // Region: SI-06 - Scope.set on already-active scope returns ERR
void test_si_06_set_on_active_scope_returns_err(void) {
    /*
     * Each scope carries a prev pointer.  Arena.create() writes the
     * current R7 into arena->prev.  While arena->prev != NULL the scope
     * is "active in the chain" and Scope.set(arena) must return ERR —
     * preventing double-activation and circular prev references.
     * After disposal prev is cleared, so the slot is reusable.
     */
    scope slb0 = Allocator.Scope.current();
    Assert.isNotNull(slb0, "SI-06: initial scope must be valid");

    scope arena = Allocator.Arena.create("si06", SCOPE_POLICY_DYNAMIC);
    Assert.isNotNull(arena, "SI-06: arena must be created");
    Assert.isTrue(Allocator.Scope.current() == arena, "SI-06: arena is current after create");

    // arena->prev == slb0 (set by create), so it is already active.
    // A second Scope.set(arena) must fail.
    integer rc = Allocator.Scope.set(arena);
    Assert.isTrue(rc == ERR,   "SI-06: set on already-active scope must return ERR");
    Assert.isTrue(Allocator.Scope.current() == arena, "SI-06: R7 unchanged after failed set");

    // System remains usable; normal dispose restores R7 to slb0
    Allocator.Arena.dispose(arena);
    Assert.isTrue(Allocator.Scope.current() == slb0, "SI-06: slb0 restored after dispose");
}
#endif

__attribute__((constructor)) void init_test(void) {
    testset("Scope Interface: R7 Stack", set_config, set_teardown);
    testcase("SI-01: current() returns SLB0 at startup", test_si_01_current_is_slb0_at_startup);
    testcase("SI-02: Arena.create sets R7 automatically", test_si_02_create_arena_sets_r7);
    testcase("SI-03: Arena.dispose restores R7", test_si_03_dispose_arena_restores_r7);
    testcase("SI-04: Scope.set pushes R7 stack", test_si_04_scope_set_pushes_r7);
    testcase("SI-05: Scope.restore pops R7 LIFO", test_si_05_scope_restore_is_lifo);
    testcase("SI-06: set on already-active scope returns ERR", test_si_06_set_on_active_scope_returns_err);
}
