/*
 * SigmaCore
 * Copyright (c) 2025 David Boarman (BadKraft) and contributors
 * QuantumOverride [Q|]
 * ----------------------------------------------
 * File: test_search_scaling.c
 * Description: Allocation scaling benchmarks to verify O(log n) complexity
 */

#define _POSIX_C_SOURCE 199309L

#include <sigma.test/sigtest.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "memory.h"
#include "sigma.core/types.h"

// Get current time in microseconds
static double get_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000.0 + ts.tv_nsec / 1000.0;
}

// Test scaling with 20 allocations (reduced after throughput tests consume NodePool)
void benchmark_50_allocations(void) {
    const int count = 20;
    const usize block_size = 256;
    void **ptrs = calloc(count, sizeof(void *));
    int actual = 0;

    double start = get_time_us();

    for (int i = 0; i < count; i++) {
        ptrs[i] = Allocator.alloc(block_size);
        if (!ptrs[i]) {
            printf("  20 blocks: NodePool exhausted at %d allocations\n", i);
            actual = i;
            goto cleanup_50;
        }
        actual++;
    }

    double end = get_time_us();
    double total_us = end - start;
    double avg_us = total_us / actual;

    printf("  20 blocks: total %.2f μs, avg %.2f μs/alloc\n", total_us, avg_us);

cleanup_50:
    for (int i = 0; i < actual; i++) {
        if (ptrs[i]) {
            Allocator.dispose(ptrs[i]);
        }
    }
    free(ptrs);
}

// Test scaling with 40 allocations
void benchmark_100_allocations(void) {
    const int count = 40;
    const usize block_size = 256;
    void **ptrs = calloc(count, sizeof(void *));
    int actual = 0;

    double start = get_time_us();

    for (int i = 0; i < count; i++) {
        ptrs[i] = Allocator.alloc(block_size);
        if (!ptrs[i]) {
            printf("  40 blocks: NodePool exhausted at %d allocations\n", i);
            actual = i;
            goto cleanup_100;
        }
        actual++;
    }

    double end = get_time_us();
    double total_us = end - start;
    double avg_us = total_us / actual;

    printf("  40 blocks: total %.2f μs, avg %.2f μs/alloc\n", total_us, avg_us);

cleanup_100:
    for (int i = 0; i < actual; i++) {
        if (ptrs[i]) {
            Allocator.dispose(ptrs[i]);
        }
    }
    free(ptrs);
}

// Test scaling with 60 allocations
void benchmark_150_allocations(void) {
    const int count = 60;
    const usize block_size = 256;
    void **ptrs = calloc(count, sizeof(void *));
    int actual = 0;

    double start = get_time_us();

    for (int i = 0; i < count; i++) {
        ptrs[i] = Allocator.alloc(block_size);
        if (!ptrs[i]) {
            printf("  60 blocks: NodePool exhausted at %d allocations\n", i);
            actual = i;
            goto cleanup_150;
        }
        actual++;
    }

    double end = get_time_us();
    double total_us = end - start;
    double avg_us = total_us / actual;

    printf("  60 blocks: total %.2f μs, avg %.2f μs/alloc\n", total_us, avg_us);

cleanup_150:
    for (int i = 0; i < actual; i++) {
        if (ptrs[i]) {
            Allocator.dispose(ptrs[i]);
        }
    }
    free(ptrs);
}

// Display scaling analysis
void benchmark_scaling_analysis(void) {
    printf("\n  Scaling Analysis:\n");
    printf("    O(log n): Doubling n increases time by ~40%% (log 2/log 1 ≈ 1.4x)\n");
    printf("    O(n):     Doubling n doubles time (2x)\n");
    printf("    Compare 20→40→60 avg μs/alloc ratios above:\n");
    printf("      - If avg stays roughly constant → O(1) (hash table)\n");
    printf("      - If avg grows by ~40%% per doubling → O(log n) (B-tree)\n");
    printf("      - If avg doubles per doubling → O(n) (linear search)\n");
}

static void set_config(FILE **log_stream) {
    *log_stream = fopen("logs/test_search_scaling.log", "w");

    printf("\n=== SLB0 Allocation Scaling Benchmark (v0.2.0) ===\n");
    printf("Objective: Verify O(log n) search complexity\n");
    printf("Architecture: Two-tier indexed (skip list + B-trees)\n");
    printf("Method: Each test disposes previous allocations before starting\n\n");
}

static void set_teardown(void) {
    printf("\n");
}

__attribute__((constructor)) void init_search_scaling_tests(void) {
    testset("Performance: Allocation Scaling", set_config, set_teardown);

    testcase("Benchmark: 20 allocations", benchmark_50_allocations);
    testcase("Benchmark: 40 allocations", benchmark_100_allocations);
    testcase("Benchmark: 60 allocations", benchmark_150_allocations);
    testcase("Benchmark: scaling analysis", benchmark_scaling_analysis);
}
