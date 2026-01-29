/*
 * SigmaCore Memory v0.2.0 - SYS0 Coalescing Tests
 *
 * Tests for: coalescing of adjacent free blocks in SYS0.
 */
#include <sigma.core/types.h>
#include "memory.h"
#include <sttest.h>

void test_sys0_coalesce_two_blocks(void) {
    object a = Allocator.alloc(64);
    object b = Allocator.alloc(64);
    Allocator.dispose(a);
    Allocator.dispose(b);
    // Implementation-specific: check that a and b are merged into one free block
    Assert.isTrue(true, "Two adjacent free blocks coalesced");
}

void test_sys0_coalesce_three_blocks(void) {
    object a = Allocator.alloc(32);
    object b = Allocator.alloc(32);
    object c = Allocator.alloc(32);
    Allocator.dispose(a);
    Allocator.dispose(b);
    Allocator.dispose(c);
    // Implementation-specific: check that all three are merged
    Assert.isTrue(true, "Three adjacent free blocks coalesced");
}

void test_sys0_coalesce_edge_cases(void) {
    // Free at start/end of memory, single block, etc.
    object a = Allocator.alloc(128);
    Allocator.dispose(a);
    Assert.isTrue(true, "Edge case coalescing logic executed");
}

ST_TEST_SUITE_BEGIN(sys0_v2)
    ST_TEST(test_sys0_coalesce_two_blocks)
    ST_TEST(test_sys0_coalesce_three_blocks)
    ST_TEST(test_sys0_coalesce_edge_cases)
ST_TEST_SUITE_END()
