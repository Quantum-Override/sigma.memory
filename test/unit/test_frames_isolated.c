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
    // Print initial state
    printf("Starting deep nesting test...\n");
    printf("Initial frame depth: %zu\n", Allocator.frame_depth());

    frame frames[8];
    for (int i = 0; i < 8; i++) {
        printf("Creating frame #%d...\n", i + 1);
        frames[i] = Allocator.frame_begin();

        if (frames[i] == NULL) {
            printf("frame_begin() returned NULL at iteration %d\n", i + 1);
            printf("Current frame_depth: %zu\n", Allocator.frame_depth());
            Assert.isNotNull(frames[i], "Frame #%d should succeed", i + 1);
        }

        printf("  Frame #%d created, depth now: %zu\n", i + 1, Allocator.frame_depth());

        object ptr = Allocator.alloc(128 * (i + 1));
        Assert.isNotNull(ptr, "Allocation in frame #%d should succeed", i + 1);
        printf("  Allocated %d bytes\n", 128 * (i + 1));
    }

    printf("All 8 frames created successfully\n");

    // Cleanup
    for (int i = 7; i >= 0; i--) {
        Allocator.frame_end(frames[i]);
        printf("Ended frame #%d, depth now: %zu\n", i + 1, Allocator.frame_depth());
    }
}

__attribute__((constructor)) void init_test(void) {
    testset("Frame Isolation Debug", set_config, set_teardown);
    testcase("Deep nesting isolated", test_deep_nesting_isolated);
}
