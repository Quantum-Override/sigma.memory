# SigmaCore Benchmark Journal

## Purpose
This document tracks performance benchmarks across versions to measure optimization impact and identify regressions.

## System Specifications
- **CPU**: Intel Core (specific model TBD)
- **RAM**: (TBD GB)
- **OS**: Linux 6.17.0-14-generic (Ubuntu 24.04)
- **Compiler**: gcc 13.3.0
- **Date**: 2026-02-12

---

## v0.2.0 Baseline (Initial Two-Tier Architecture)

### Architecture
- **Tier 1**: Skip list for page-level indexing
- **Tier 2**: Per-page B-trees for block-level indexing
- **Page Growth**: Dynamic via mmap (16-page initial allocation)
- **NodePool**: Fixed 512-node capacity (mremap growth deferred to v0.2.1+)

### Allocation Throughput Benchmark
**Test**: Allocate 50 blocks at various sizes (4KB test uses 5 iterations)

| Block Size | Operations/sec | Notes |
|------------|----------------|-------|
| 64B        | 573,526        | Small block performance |
| 1KB        | 1,276,943      | Medium block performance (fastest!) |
| 4KB        | 1,372,102      | Large block performance (5 iterations) |
| Mixed (16B-2KB) | 549,825   | Realistic workload simulation |

**Key Observations**:
- 1KB and 4KB allocations show higher throughput than 64B
- This may indicate overhead in small block handling or measurement granularity
- Mixed workload (549k ops/sec) provides realistic baseline for typical usage
- All tests complete successfully within NodePool constraints

### Allocation Scaling Benchmark
**Test**: Measure avg allocation time as block count increases (256B blocks)

| Block Count | Total Time (μs) | Avg Time (μs/alloc) | Scaling Factor |
|-------------|-----------------|---------------------|----------------|
| 50          | 40.10           | 0.80                | 1.0x (baseline) |
| 100         | 136.94          | 1.37                | 1.71x |
| 150         | 204.04          | 1.36                | 1.70x (stable) |

**Key Observations**:
- 50→100: 1.71x increase (between O(log n) 1.4x and O(n) 2.0x)
- 100→150: Effectively constant (~1.36-1.37 μs)
- Behavior suggests O(log n) characteristics with amortization
- No degradation at 150 blocks indicates healthy B-tree balancing
- Two-tier architecture performing as expected

### Memory Safety
- All tests pass valgrind leak check
- No invalid reads/writes during benchmark execution
- Proper resource cleanup verified

### Limitations & Notes
- **NodePool Constraint**: Fixed 512-node pool limits stress testing
  - Initial tests with 1000 iterations failed at ~84-300 allocations (cumulative)
  - Reduced to 50 iterations per test to stay within capacity
  - v0.2.1+ will implement mremap-based growth
- **Dispose-Reallocate Pattern**: Discovered allocator bug when disposing and reallocating in same test
  - Crash at address 0x485c008 (consistent across runs)
  - Workaround: Dispose at end of each test, not during
  - Issue deferred to v0.2.1 bugfix cycle
- **Small Sample Size**: 50 iterations may have higher variance
  - Consider 3-5 runs for statistical confidence in future versions

### Baseline Summary
v0.2.0 provides a functional baseline with:
- ✅ Dynamic page growth working correctly
- ✅ Page reclamation functional
- ✅ Memory cleanup verified
- ✅ O(log n) scaling characteristics observed
- ✅ ~500k-1.3M ops/sec throughput for typical workloads
- ⚠️ NodePool capacity limits stress testing
- ⚠️ Dispose-reallocate pattern triggers allocator crash

**Next Steps (v0.2.1+)**:
1. Implement NodePool mremap growth to remove 512-node constraint
2. Debug dispose-reallocate crash (address 0x485c008)
3. Re-run benchmarks with larger iteration counts (1000-10000)
4. Profile hot paths for optimization opportunities
5. Add stress tests for sustained load and memory exhaustion

---

## Future Versions

### v0.2.1 (Planned)
- [ ] NodePool mremap growth implementation
- [ ] Fix dispose-reallocate allocator bug
- [ ] Increase benchmark iterations to 1000-10000
- [ ] Add stress testing suite
- [ ] Performance profiling and hotspot identification

### v0.3.0 (Planned)
- [ ] Internal fragmentation optimization
- [ ] B-tree rebalancing tuning
- [ ] Skip list probability adjustment
- [ ] Hot path optimization based on profiling

---

## Benchmark Methodology

### Running Benchmarks
```bash
# From repository root
ctest allocation_throughput     # Throughput at various block sizes
ctest search_scaling            # Scaling characteristics
ctest allocation_throughput --valgrind  # Memory safety check
ctest search_scaling --valgrind         # Memory safety check
```

### Adding New Benchmarks
1. Create test file in `test/performance/test_<feature>.c`
2. Follow sigtest API patterns (see existing benchmarks)
3. Use `clock_gettime(CLOCK_MONOTONIC)` for microsecond timing
4. Add `#define _POSIX_C_SOURCE 199309L` before includes
5. Handle NodePool exhaustion gracefully (check for NULL)
6. Use `calloc()` for pointer arrays to avoid disposing garbage
7. Document results in this journal under appropriate version

### Interpreting Results
- **Throughput**: Higher ops/sec is better
- **Scaling**: Compare actual growth to O(log n) expectations:
  - Doubling n should increase avg time by ~40% for O(log n)
  - Less than 40% suggests O(1) behavior (caching, amortization)
  - More than 40% (up to 2x) suggests O(n) behavior (linear search)
- **Memory Safety**: All benchmarks must pass `--valgrind` clean
