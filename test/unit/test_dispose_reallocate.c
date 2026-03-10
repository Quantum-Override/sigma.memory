/*
 * SigmaCore
 * Copyright (c) 2026 David Boarman (BadKraft) and contributors
 * QuantumOverride [Q|]
 * ----------------------------------------------
 * File: test_dispose_reallocate.c
 * Description: Minimal test case for dispose-reallocate crash
 */

#include <stdio.h>
#include <sigma.test/sigtest.h>
#include "memory.h"
#include "sigma.core/types.h"

// Minimal reproducer: allocate → dispose → reallocate
void test_dispose_reallocate_single(void) {
    void *ptrs[50];
    
    // Allocate 50 blocks (like benchmark)
    for (int i = 0; i < 50; i++) {
        ptrs[i] = Allocator.alloc(256);
        Assert.isNotNull(ptrs[i], "Allocation %d should succeed", i);
    }
    
    // Dispose all
    for (int i = 0; i < 50; i++) {
        Allocator.dispose(ptrs[i]);
    }
}

// Test reallocating after disposing multiple blocks
void test_dispose_reallocate_multiple(void) {
    void *ptrs[50];
    
    // This runs AFTER previous test disposed 50 blocks
    // Try to reallocate - should reuse freed blocks
    for (int i = 0; i < 50; i++) {
        ptrs[i] = Allocator.alloc(256);
        Assert.isNotNull(ptrs[i], "Reallocation %d should succeed", i);
    }
    
    // Dispose all
    for (int i = 0; i < 50; i++) {
        Allocator.dispose(ptrs[i]);
    }
}

// Test coalescing pattern
void test_dispose_reallocate_coalesce(void) {
    // Allocate 3 adjacent blocks  
    void *ptr1 = Allocator.alloc(128);
    void *ptr2 = Allocator.alloc(128);
    void *ptr3 = Allocator.alloc(128);
    
    Assert.isNotNull(ptr1, "Block 1 should allocate");
    Assert.isNotNull(ptr2, "Block 2 should allocate");
    Assert.isNotNull(ptr3, "Block 3 should allocate");
    
    // Dispose middle block (should NOT coalesce yet)
    Allocator.dispose(ptr2);
    
    // Dispose first block (should coalesce with ptr2)
    Allocator.dispose(ptr1);
    
    // Try to reallocate larger block (should fit in coalesced space)
    void *big_ptr = Allocator.alloc(256);
    Assert.isNotNull(big_ptr, "Coalesced block should allocate");
    
    // Cleanup
    if (big_ptr) {
        Allocator.dispose(big_ptr);
    }
    if (ptr3) {
        Allocator.dispose(ptr3);
    }
}

static void set_config(FILE **log_stream) {
    *log_stream = fopen("logs/test_dispose_reallocate.log", "w");
}

static void set_teardown(void) {
    // Cleanup
}

__attribute__((constructor)) void init_dispose_reallocate_tests(void) {
    testset("Unit: Dispose-Reallocate Bug", set_config, set_teardown);
    
    testcase("Reproduce: single dispose-reallocate", test_dispose_reallocate_single);
    testcase("Reproduce: multiple dispose-reallocate", test_dispose_reallocate_multiple);
    testcase("Reproduce: coalesce pattern", test_dispose_reallocate_coalesce);
}
