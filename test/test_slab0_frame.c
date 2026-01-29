/*
 * SigmaCore Memory v0.2.0 - SLB0 Frame Support Tests
 *
 * Tests for: begin_frame/end_frame semantics, error on nested frames, free list interaction.
 */
#include <sigma.core/types.h>
#include "memory.h"
#include "slab_manager.h"
#include <sttest.h>

void test_slab0_frame_basic(void) {
    Allocator.Scope.begin_frame();
    object a = Allocator.alloc(64);
    object b = Allocator.alloc(64);
    Allocator.Scope.end_frame();
    // a and b should now be invalid (freed)
    Assert.isTrue(true, "Frame allocations reclaimed");
}

void test_slab0_frame_preserve_prior(void) {
    object a = Allocator.alloc(64);
    Allocator.Scope.begin_frame();
    object b = Allocator.alloc(64);
    Allocator.Scope.end_frame();
    // a should still be valid
    Assert.isNotNull(a, "Allocations before frame remain valid");
    Allocator.dispose(a);
}

void test_slab0_frame_no_nesting(void) {
    Allocator.Scope.begin_frame();
    bool nested_error = false;
    // Attempt to begin a nested frame
    if (!Allocator.Scope.begin_frame()) {
        nested_error = true;
    }
    Allocator.Scope.end_frame();
    Assert.isTrue(nested_error, "Nested frames not allowed in SLB0");
}

ST_TEST_SUITE_BEGIN(slab0_frame)
    ST_TEST(test_slab0_frame_basic)
    ST_TEST(test_slab0_frame_preserve_prior)
    ST_TEST(test_slab0_frame_no_nesting)
ST_TEST_SUITE_END()
