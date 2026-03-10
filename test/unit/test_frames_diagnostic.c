/*
 * Diagnostic test to understand resource exhaustion
 */

#include <sigma.test/sigtest.h>
#include "memory.h"
#include <stdio.h>

static void set_config(FILE **log_stream) {
    *log_stream = fopen("logs/test_frames_diagnostic.log", "w");
}

static void set_teardown(void) {}

void test_sequential_frames_with_diagnostics(void) {
    printf("\n=== Sequential Frame Test ===\n");
    
    // Run frame operations sequentially, checking state after each
    for (int i = 0; i < 15; i++) {
        printf("\n--- Iteration %d ---\n", i + 1);
        
        frame f = Allocator.frame_begin();
        if (f == NULL) {
            printf("FAILED: frame_begin() returned NULL on iteration %d\n", i + 1);
            printf("Frame depth: %zu\n", Allocator.frame_depth());
            Assert.isNotNull(f, "Frame %d should succeed", i + 1);
        }
        
        printf("Created frame, depth=%zu\n", Allocator.frame_depth());
        
        // Make a few allocations
        object ptr1 = Allocator.alloc(256);
        object ptr2 = Allocator.alloc(512);
        object ptr3 = Allocator.alloc(1024);
        
        if (!ptr1 || !ptr2 || !ptr3) {
            printf("FAILED: Allocation failed in iteration %d\n", i + 1);
            Assert.isNotNull(ptr1, "Alloc 1 should succeed");
            Assert.isNotNull(ptr2, "Alloc 2 should succeed");
            Assert.isNotNull(ptr3, "Alloc 3 should succeed");
        }
        
        usize allocated = Allocator.frame_allocated(f);
        printf("Allocated %zu bytes in frame\n", allocated);
        
        // End frame
        integer result = Allocator.frame_end(f);
        if (result != 0) {
            printf("FAILED: frame_end() returned %ld\n", result);
            Assert.isTrue(result == 0, "frame_end should succeed");
        }
        
        printf("Ended frame, depth=%zu\n", Allocator.frame_depth());
        
        // Verify we're back to depth 0
        Assert.isTrue(Allocator.frame_depth() == 0, "Depth should be 0 after frame_end");
    }
    
    printf("\n=== All 15 iterations completed successfully ===\n");
}

__attribute__((constructor)) void init_test(void) {
    testset("Frame Diagnostics", set_config, set_teardown);
    testcase("Sequential frames with diagnostics", test_sequential_frames_with_diagnostics);
}
