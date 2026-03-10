/*
 * SigmaCore
 * Copyright (c) 2025 David Boarman (BadKraft) and contributors
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
 * File: test_allocation_throughput.c
 * Description: Benchmark allocation throughput at various block sizes
 */

#define _POSIX_C_SOURCE 199309L

#include <sigma.test/sigtest.h>
#include <stdio.h>
#include <sys/mman.h>
#include <time.h>

#include "internal/memory.h"
#include "memory.h"

#define BENCHMARK_ITERATIONS 40  // Current limit with 2KB NodePool  + split bugfix

// Helper: Get time in microseconds
static double get_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000.0 + ts.tv_nsec / 1000.0;
}

// Measure allocations/sec for 64-byte blocks
void benchmark_alloc_64B(void) {
    const usize block_size = 64;
    void *ptrs[BENCHMARK_ITERATIONS];

    double start = get_time_us();

    for (int i = 0; i < BENCHMARK_ITERATIONS; i++) {
        ptrs[i] = Allocator.alloc(block_size);
        Assert.isNotNull(ptrs[i], "Allocation %d should succeed", i);
    }

    double end = get_time_us();
    double elapsed_ms = (end - start) / 1000.0;
    double ops_per_sec = BENCHMARK_ITERATIONS / (elapsed_ms / 1000.0);

    // Cleanup
    for (int i = 0; i < BENCHMARK_ITERATIONS; i++) {
        if (ptrs[i]) {
            Allocator.dispose(ptrs[i]);
        }
    }

    printf("  64B blocks: %.0f ops/sec (%.3f ms total)\n", ops_per_sec, elapsed_ms);
}

// Measure allocations/sec for 1KB blocks
void benchmark_alloc_1KB(void) {
    const usize block_size = 1024;
    void *ptrs[BENCHMARK_ITERATIONS];

    double start = get_time_us();

    for (int i = 0; i < BENCHMARK_ITERATIONS; i++) {
        ptrs[i] = Allocator.alloc(block_size);
        Assert.isNotNull(ptrs[i], "Allocation %d should succeed", i);
    }

    double end = get_time_us();
    double elapsed_ms = (end - start) / 1000.0;
    double ops_per_sec = BENCHMARK_ITERATIONS / (elapsed_ms / 1000.0);

    // Cleanup
    for (int i = 0; i < BENCHMARK_ITERATIONS; i++) {
        if (ptrs[i]) {
            Allocator.dispose(ptrs[i]);
        }
    }

    printf("  1KB blocks: %.0f ops/sec (%.3f ms total)\n", ops_per_sec, elapsed_ms);
}

// Measure allocations/sec for 4KB blocks (single page size)
void benchmark_alloc_4KB(void) {
    const usize block_size = 4096;
    const int iterations = BENCHMARK_ITERATIONS / 10;  // Fewer iterations for large blocks
    void *ptrs[BENCHMARK_ITERATIONS / 10];

    double start = get_time_us();

    for (int i = 0; i < iterations; i++) {
        ptrs[i] = Allocator.alloc(block_size);
        Assert.isNotNull(ptrs[i], "Allocation %d should succeed", i);
    }

    double end = get_time_us();
    double elapsed_ms = (end - start) / 1000.0;
    double ops_per_sec = iterations / (elapsed_ms / 1000.0);

    // Cleanup
    for (int i = 0; i < iterations; i++) {
        if (ptrs[i]) {
            Allocator.dispose(ptrs[i]);
        }
    }

    printf("  4KB blocks: %.0f ops/sec (%.3f ms total, %d iterations)\n", ops_per_sec, elapsed_ms,
           iterations);
}

// Measure realistic workload with varied block sizes
void benchmark_mixed_sizes(void) {
    const int iterations = BENCHMARK_ITERATIONS;
    void *ptrs[BENCHMARK_ITERATIONS];
    const usize sizes[] = {16, 32, 64, 128, 256, 512, 1024, 2048};
    const int num_sizes = sizeof(sizes) / sizeof(sizes[0]);

    double start = get_time_us();

    for (int i = 0; i < iterations; i++) {
        usize size = sizes[i % num_sizes];
        ptrs[i] = Allocator.alloc(size);
        Assert.isNotNull(ptrs[i], "Allocation %d (size %zu) should succeed", i, size);
    }

    double end = get_time_us();
    double elapsed_ms = (end - start) / 1000.0;
    double ops_per_sec = iterations / (elapsed_ms / 1000.0);

    // Cleanup
    for (int i = 0; i < iterations; i++) {
        if (ptrs[i]) {
            Allocator.dispose(ptrs[i]);
        }
    }

    printf("  Mixed sizes (16B-2KB): %.0f ops/sec (%.3f ms total)\n", ops_per_sec, elapsed_ms);
}

static void set_config(FILE **log_stream) {
    *log_stream = fopen("logs/test_allocation_throughput.log", "w");

    // Warmup: Ensure memory system initialized
    (void)Memory.state();

    printf("\n=== SLB0 Allocation Throughput Benchmark (v0.2.0) ===\n");
    printf("Iterations: %d allocations per size\n", BENCHMARK_ITERATIONS);
    printf("Architecture: Two-tier indexed (skip list + B-trees)\n\n");
}

static void set_teardown(void) {
    printf("\n");
}

__attribute__((constructor)) void init_allocation_throughput_tests(void) {
    testset("Performance: Allocation Throughput", set_config, set_teardown);

    testcase("Benchmark: 64B allocations", benchmark_alloc_64B);
    testcase("Benchmark: 1KB allocations", benchmark_alloc_1KB);
    testcase("Benchmark: 4KB allocations", benchmark_alloc_4KB);
    testcase("Benchmark: mixed sizes", benchmark_mixed_sizes);
}
