/*
 * Test isolated frame scenario to debug resource exhaustion
 */

#include <sigtest/sigtest.h>
#include <stdio.h>
#include "memory.h"

static void set_config(FILE **log_stream) {
    *log_stream = fopen("logs/test_frames_isolated.log", "w");
}

static void set_teardown(void) {
}

void test_deep_nesting_isolated(void) {
    // v0.2.3: single-frame-per-scope model
    // Verify: 8 sequential frames each open/alloc/close independently
    printf("Starting sequential frame isolation test...\n");
    printf("Initial frame depth: %zu\n", Allocator.frame_depth());
    Assert.isTrue(Allocator.frame_depth() == 0, "Should start with no active frame");

    for (int i = 0; i < 8; i++) {
        printf("Opening frame #%d...\n", i + 1);
        frame f = Allocator.frame_begin();

        if (f == NULL) {
            printf("frame_begin() returned NULL at iteration %d (depth=%zu)\n",
                   i + 1, Allocator.frame_depth());
            Assert.isNotNull(f, "Frame #%d should succeed (sequential)", i + 1);
            return;  // early exit to avoid cascade
        }

        printf("  Frame #%d created, depth now: %zu\n", i + 1, Allocator.frame_depth());
        Assert.isTrue(Allocator.frame_depth() == 1, "Depth should be 1 for frame #%d", i + 1);

        // Attempt a nested frame — must return NULL (single-frame model)
        frame nested = Allocator.frame_begin();
        Assert.isNull(nested, "Nested begin() while frame #%d active should return NULL", i + 1);
        Assert.isTrue(Allocator.frame_depth() == 1, "Depth stays 1 with duplicate begin");

        object ptr = Allocator.alloc(128 * (i + 1));
        Assert.isNotNull(ptr, "Allocation in frame #%d should succeed", i + 1);
        printf("  Allocated %d bytes\n", 128 * (i + 1));

        integer result = Allocator.frame_end(f);
        Assert.isTrue(result == OK, "frame_end() for frame #%d should succeed", i + 1);
        printf("  Ended frame #%d, depth now: %zu\n", i + 1, Allocator.frame_depth());
        Assert.isTrue(Allocator.frame_depth() == 0, "Depth should be 0 after frame #%d", i + 1);
    }

    printf("All 8 sequential frames completed successfully\n");
}

__attribute__((constructor)) void init_test(void) {
    testset("Frame Isolation Debug", set_config, set_teardown);
    testcase("Sequential frame isolation (8 rounds)", test_deep_nesting_isolated);
}
