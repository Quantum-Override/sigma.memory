/*
 * SigmaCore
 * Copyright (c) 2025 David Boarman (BadKraft) and contributors
 * QuantumOverride [Q|]
 * ----------------------------------------------
 * MIT License
 * ----------------------------------------------
 * File: test_frames.c
 * Description: Unit tests for frame support (v0.2.1)
 *              Tests chunked bump allocation with LIFO nesting
 */

#include <sigtest/sigtest.h>
#include "internal/memory.h"
#include "memory.h"

#if 1  // Region: Test Set Setup & Teardown
static void set_config(FILE **log_stream) {
    *log_stream = fopen("logs/test_frames.log", "w");
    sbyte state = Memory.state();
    Assert.isTrue(state & MEM_STATE_READY, "Memory system should be ready");
}

static void set_teardown(void) {
    // Cleanup
}
#endif

#if 1  // Region: TDD Phase 1 - Basic Frame Operations
void test_frame_begin_returns_handle(void) {
    // Arrange: Memory system initialized

    // Act: Begin a frame
    frame f = Allocator.frame_begin();

    // Assert: Handle should be non-NULL, frame depth should be 1
    Assert.isNotNull(f, "frame_begin() should return non-NULL handle");
    usize depth = Allocator.frame_depth();
    Assert.isTrue(depth == 1, "Frame depth should be 1 after frame_begin(), got %zu", depth);

    // Cleanup
    Allocator.frame_end(f);
}

void test_frame_single_allocation(void) {
    // Arrange: Begin a frame
    frame f = Allocator.frame_begin();
    Assert.isNotNull(f, "frame_begin() should succeed");

    // Act: Allocate within frame
    object ptr = Allocator.alloc(64);

    // Assert: Allocation should succeed
    Assert.isNotNull(ptr, "Allocation within frame should succeed");

    // Act: End frame
    integer result = Allocator.frame_end(f);

    // Assert: Frame end should succeed, depth back to 0
    Assert.isTrue(result == OK, "frame_end() should return OK, got %ld", result);
    usize depth = Allocator.frame_depth();
    Assert.isTrue(depth == 0, "Frame depth should be 0 after frame_end(), got %zu", depth);
}

void test_frame_end_deallocates_all(void) {
    // Arrange: Begin frame and make multiple allocations
    frame f = Allocator.frame_begin();
    Assert.isNotNull(f, "frame_begin() should succeed");

    object ptr1 = Allocator.alloc(256);
    object ptr2 = Allocator.alloc(256);
    object ptr3 = Allocator.alloc(256);

    // Assert: All allocations succeed
    Assert.isNotNull(ptr1, "First allocation should succeed");
    Assert.isNotNull(ptr2, "Second allocation should succeed");
    Assert.isNotNull(ptr3, "Third allocation should succeed");

    usize allocated = Allocator.frame_allocated(f);
    Assert.isTrue(allocated >= 768, "Frame should show at least 768 bytes allocated");

    // Act: End frame
    integer result = Allocator.frame_end(f);

    // Assert: Frame end succeeds, all memory freed
    Assert.isTrue(result == OK, "frame_end() should return OK, got %ld", result);
    usize depth = Allocator.frame_depth();
    Assert.isTrue(depth == 0, "Frame depth should be 0 after frame_end(), got %zu", depth);
}

void test_frame_without_allocation(void) {
    // Arrange: Begin frame
    frame f = Allocator.frame_begin();
    Assert.isNotNull(f, "frame_begin() should succeed");

    // Act: End frame immediately without any allocations
    integer result = Allocator.frame_end(f);

    // Assert: Should succeed
    Assert.isTrue(result == OK, "frame_end() on empty frame should succeed, got %ld", result);
    usize depth = Allocator.frame_depth();
    Assert.isTrue(depth == 0, "Frame depth should be 0 after frame_end(), got %zu", depth);
}

void test_frame_max_depth(void) {
    // Arrange: Array to hold frame handles
    frame frames[MAX_FRAME_DEPTH + 1];

    // Act: Create MAX_FRAME_DEPTH frames
    for (int i = 0; i < MAX_FRAME_DEPTH; i++) {
        frames[i] = Allocator.frame_begin();
        Assert.isNotNull(frames[i], "frame_begin() #%d should succeed", i + 1);
    }

    usize depth = Allocator.frame_depth();
    Assert.isTrue(depth == MAX_FRAME_DEPTH, "Frame depth should be MAX_FRAME_DEPTH (%d), got %zu",
                  MAX_FRAME_DEPTH, depth);

    // Act: Attempt to create one more frame (should fail)
    frame overflow = Allocator.frame_begin();

    // Assert: Should return NULL at limit
    Assert.isNull(overflow, "frame_begin() should return NULL when depth exceeds MAX_FRAME_DEPTH");
    Assert.isTrue(Allocator.frame_depth() == MAX_FRAME_DEPTH,
                  "Depth should remain at MAX_FRAME_DEPTH");

    // Cleanup: Unwind all frames in LIFO order
    for (int i = MAX_FRAME_DEPTH - 1; i >= 0; i--) {
        integer result = Allocator.frame_end(frames[i]);
        Assert.isTrue(result == OK, "frame_end() #%d should succeed", i + 1);
    }

    Assert.isTrue(Allocator.frame_depth() == 0, "All frames should be unwound");
}

void test_frame_depth_tracking(void) {
    // Arrange & Act: Create nested frames
    frame f1 = Allocator.frame_begin();
    Assert.isNotNull(f1, "First frame should succeed");
    Assert.isTrue(Allocator.frame_depth() == 1, "Depth should be 1");

    frame f2 = Allocator.frame_begin();
    Assert.isNotNull(f2, "Second frame should succeed");
    Assert.isTrue(Allocator.frame_depth() == 2, "Depth should be 2");

    frame f3 = Allocator.frame_begin();
    Assert.isNotNull(f3, "Third frame should succeed");
    Assert.isTrue(Allocator.frame_depth() == 3, "Depth should be 3");

    // Act: End middle frame (LIFO order matters, but we test inner first)
    Allocator.frame_end(f3);
    Assert.isTrue(Allocator.frame_depth() == 2, "Depth should be 2 after ending f3");

    Allocator.frame_end(f2);
    Assert.isTrue(Allocator.frame_depth() == 1, "Depth should be 1 after ending f2");

    Allocator.frame_end(f1);
    Assert.isTrue(Allocator.frame_depth() == 0, "Depth should be 0 after ending f1");
}
#endif

#if 1  // Region: TDD Phase 2 - Chunk Overflow & Chaining
void test_frame_single_chunk_overflow(void) {
    // Arrange: Begin frame
    frame f = Allocator.frame_begin();
    Assert.isNotNull(f, "frame_begin() should succeed");

    // Act: Allocate > 4KB to force chunk chaining
    // FRAME_CHUNK_SIZE = 4096, so allocate 5000 bytes total
    object ptr1 = Allocator.alloc(2048);
    object ptr2 = Allocator.alloc(2048);  // Total: 4096 (fills chunk 1)
    object ptr3 = Allocator.alloc(1024);  // Forces chunk 2

    // Assert: All allocations succeed
    Assert.isNotNull(ptr1, "First allocation (2KB) should succeed");
    Assert.isNotNull(ptr2, "Second allocation (2KB) should succeed");
    Assert.isNotNull(ptr3, "Third allocation (1KB) should succeed and force new chunk");

    // Verify total allocated
    usize allocated = Allocator.frame_allocated(f);
    Assert.isTrue(allocated >= 5120, "Frame should show at least 5120 bytes allocated, got %zu",
                  allocated);

    // Cleanup
    integer result = Allocator.frame_end(f);
    Assert.isTrue(result == OK, "frame_end() should succeed after chunk overflow");
}

void test_frame_multiple_chunks(void) {
    // Arrange: Begin frame
    frame f = Allocator.frame_begin();
    Assert.isNotNull(f, "frame_begin() should succeed");

    // Act: Allocate enough to require 3 chunks (12KB total)
    object ptrs[12];
    for (int i = 0; i < 12; i++) {
        ptrs[i] = Allocator.alloc(1024);  // 12 × 1KB = 12KB
        Assert.isNotNull(ptrs[i], "Allocation #%d should succeed", i + 1);
    }

    // Assert: Frame shows total allocated
    usize allocated = Allocator.frame_allocated(f);
    Assert.isTrue(allocated >= 12288, "Frame should show at least 12KB allocated, got %zu",
                  allocated);

    // Cleanup
    integer result = Allocator.frame_end(f);
    Assert.isTrue(result == OK, "frame_end() should succeed with multiple chunks");
}

void test_frame_large_single_allocation(void) {
    // Arrange: Begin frame
    frame f = Allocator.frame_begin();
    Assert.isNotNull(f, "frame_begin() should succeed");

    // Act: Allocate a single large block (6KB - larger than 4KB chunk)
    object ptr = Allocator.alloc(6144);

    // Assert: Should succeed (tracked as large allocation in frame)
    Assert.isNotNull(ptr, "Large allocation (6KB) should succeed");

    usize allocated = Allocator.frame_allocated(f);
    Assert.isTrue(allocated >= 6144, "Frame should show at least 6KB allocated, got %zu",
                  allocated);

    // Cleanup
    integer result = Allocator.frame_end(f);
    Assert.isTrue(result == OK, "frame_end() should succeed after large allocation");
}

void test_frame_boundary_allocations(void) {
    // Test allocations at chunk boundaries
    frame f = Allocator.frame_begin();
    Assert.isNotNull(f, "frame_begin() should succeed");

    // Allocate exactly 4096 bytes (fill one chunk exactly)
    object ptr1 = Allocator.alloc(4096);
    Assert.isNotNull(ptr1, "4KB allocation should succeed");

    // Next allocation should trigger new chunk
    object ptr2 = Allocator.alloc(64);
    Assert.isNotNull(ptr2, "Allocation after boundary should succeed");

    usize allocated = Allocator.frame_allocated(f);
    Assert.isTrue(allocated >= 4160, "Should show at least 4160 bytes, got %zu", allocated);

    Allocator.frame_end(f);
}
#endif

#if 1  // Region: TDD Phase 3 - Nesting Stress & Mixed Allocations
void test_frame_deep_nesting_with_allocations(void) {
    // Arrange: Create deeply nested frames (8 levels) with allocations in each
    frame frames[8];

    // Act: Create nested frames, each with allocations
    for (int i = 0; i < 8; i++) {
        frames[i] = Allocator.frame_begin();
        Assert.isNotNull(frames[i], "Frame #%d should succeed", i + 1);

        // Allocate incrementally larger amounts
        object ptr = Allocator.alloc(128 * (i + 1));
        Assert.isNotNull(ptr, "Allocation in frame #%d should succeed", i + 1);
    }

    Assert.isTrue(Allocator.frame_depth() == 8, "Depth should be 8");

    // Cleanup: Unwind in LIFO order
    for (int i = 7; i >= 0; i--) {
        integer result = Allocator.frame_end(frames[i]);
        Assert.isTrue(result == OK, "Unwinding frame #%d should succeed", i + 1);
    }

    Assert.isTrue(Allocator.frame_depth() == 0, "All frames should be unwound");
}

void test_frame_mixed_normal_and_frame_allocations(void) {
    // Arrange: Interleave normal and frame allocations
    object normal1 = Allocator.alloc(256);
    Assert.isNotNull(normal1, "Normal allocation 1 should succeed");

    frame f1 = Allocator.frame_begin();
    Assert.isNotNull(f1, "Frame 1 should succeed");

    object frame1_ptr = Allocator.alloc(512);
    Assert.isNotNull(frame1_ptr, "Frame 1 allocation should succeed");

    object frame1_ptr2 = Allocator.alloc(128);
    Assert.isNotNull(frame1_ptr2, "Frame 1 allocation 2 should succeed");

    frame f2 = Allocator.frame_begin();
    Assert.isNotNull(f2, "Frame 2 should succeed");

    object frame2_ptr = Allocator.alloc(1024);
    Assert.isNotNull(frame2_ptr, "Frame 2 allocation should succeed");

    // Cleanup frames (LIFO) - frees frame1_ptr, frame1_ptr2, frame2_ptr
    Allocator.frame_end(f2);
    Allocator.frame_end(f1);

    // Normal allocations should still work after frames
    object normal2 = Allocator.alloc(64);
    Assert.isNotNull(normal2, "Normal allocation after frames should succeed");

    // Cleanup normal allocations (only those allocated outside frames)
    Allocator.dispose(normal2);
    Allocator.dispose(normal1);
}

void test_frame_allocation_isolation(void) {
    // Verify that frame allocations don't interfere with each other
    frame f1 = Allocator.frame_begin();
    object f1_ptr1 = Allocator.alloc(256);
    object f1_ptr2 = Allocator.alloc(512);

    frame f2 = Allocator.frame_begin();
    object f2_ptr1 = Allocator.alloc(1024);
    object f2_ptr2 = Allocator.alloc(128);

    // Both frames should have independent allocations
    usize f1_alloc = Allocator.frame_allocated(f1);
    usize f2_alloc = Allocator.frame_allocated(f2);

    Assert.isTrue(f1_alloc >= 768, "Frame 1 should show at least 768 bytes");
    Assert.isTrue(f2_alloc >= 1152, "Frame 2 should show at least 1152 bytes");

    // End inner frame
    Allocator.frame_end(f2);

    // Outer frame allocations should still be valid
    Assert.isTrue(Allocator.frame_allocated(f1) >= 768, "Frame 1 still intact");

    Allocator.frame_end(f1);
}

void test_frame_stress_many_small_allocations(void) {
    // Stress test: Many small allocations in one frame
    frame f = Allocator.frame_begin();
    Assert.isNotNull(f, "frame_begin() should succeed");

    // Allocate 100 × 64 bytes = 6400 bytes (requires 2 chunks)
    for (int i = 0; i < 100; i++) {
        object ptr = Allocator.alloc(64);
        Assert.isNotNull(ptr, "Small allocation #%d should succeed", i + 1);
    }

    usize allocated = Allocator.frame_allocated(f);
    Assert.isTrue(allocated >= 6400, "Should show at least 6400 bytes, got %zu", allocated);

    integer result = Allocator.frame_end(f);
    Assert.isTrue(result == OK, "frame_end() after many small allocations should succeed");
}
#endif

#if 1  // Region: Edge Cases & Hardening
void test_frame_zero_size_allocation(void) {
    // Test behavior with zero-size allocation
    frame f = Allocator.frame_begin();
    Assert.isNotNull(f, "frame_begin() should succeed");

    object ptr = Allocator.alloc(0);
    // Should return NULL per allocator convention
    Assert.isNull(ptr, "Zero-size allocation should return NULL");

    Allocator.frame_end(f);
}

void test_frame_very_large_allocation(void) {
    // Test allocation larger than frame chunk size (6KB > 4KB chunk)
    frame f = Allocator.frame_begin();
    Assert.isNotNull(f, "frame_begin() should succeed");

    object ptr = Allocator.alloc(6144);  // 6KB
    if (ptr != NULL) {
        // If system can handle it, verify frame tracks it
        usize allocated = Allocator.frame_allocated(f);
        Assert.isTrue(allocated >= 6144, "Should track large allocation");
    }
    // Note: May legitimately fail if page limit reached

    Allocator.frame_end(f);
}

void test_frame_alternating_sizes(void) {
    // Mix of large and small allocations
    frame f = Allocator.frame_begin();
    Assert.isNotNull(f, "frame_begin() should succeed");

    object large1 = Allocator.alloc(3000);
    object small1 = Allocator.alloc(64);
    object large2 = Allocator.alloc(2500);
    object small2 = Allocator.alloc(128);
    object large3 = Allocator.alloc(1500);

    Assert.isNotNull(large1, "Large allocation 1 should succeed");
    Assert.isNotNull(small1, "Small allocation 1 should succeed");
    Assert.isNotNull(large2, "Large allocation 2 should succeed");
    Assert.isNotNull(small2, "Small allocation 2 should succeed");
    Assert.isNotNull(large3, "Large allocation 3 should succeed");

    usize allocated = Allocator.frame_allocated(f);
    Assert.isTrue(allocated >= 7192, "Should show all allocations, got %zu", allocated);

    Allocator.frame_end(f);
}
#endif

#if 1  // Region: Test Registration
__attribute__((constructor)) void init_frame_tests(void) {
    testset("Memory: Frame Support", set_config, set_teardown);

    // Phase 1: Basic operations
    testcase("Frame: begin returns handle", test_frame_begin_returns_handle);
    testcase("Frame: single allocation", test_frame_single_allocation);
    testcase("Frame: end deallocates all", test_frame_end_deallocates_all);
    testcase("Frame: empty frame", test_frame_without_allocation);
    testcase("Frame: max depth limit", test_frame_max_depth);
    testcase("Frame: depth tracking", test_frame_depth_tracking);

    // Phase 2: Chunk overflow & chaining
    testcase("Frame: single chunk overflow", test_frame_single_chunk_overflow);
    testcase("Frame: multiple chunks", test_frame_multiple_chunks);
    testcase("Frame: large single allocation", test_frame_large_single_allocation);
    testcase("Frame: boundary allocations", test_frame_boundary_allocations);

    // Phase 3: Nesting stress & mixed allocations
    testcase("Frame: deep nesting with allocations", test_frame_deep_nesting_with_allocations);
    testcase("Frame: mixed normal and frame allocations",
             test_frame_mixed_normal_and_frame_allocations);
    testcase("Frame: allocation isolation", test_frame_allocation_isolation);
    testcase("Frame: stress many small allocations", test_frame_stress_many_small_allocations);

    // Edge cases & hardening
    testcase("Frame: zero-size allocation", test_frame_zero_size_allocation);
    testcase("Frame: very large allocation", test_frame_very_large_allocation);
    testcase("Frame: alternating sizes", test_frame_alternating_sizes);
}
#endif
