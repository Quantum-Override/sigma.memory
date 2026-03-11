# Changelog

All notable changes to Sigma.Memory are documented here. Full release notes for each version are in [`docs/archive/`](docs/archive/).

---

## [0.2.3] - 2026-03-09

**Realloc API + Dynamic Page Release + Skip List Correctness**

- Added `Allocator.realloc(ptr, size)` — standard realloc semantics with in-place shrink
- Dynamic SLB0 page release: overflow pages returned to OS via `munmap` when fully freed
- Skip list correctness: fragmented pages now excluded from large allocation requests
- Fixed critical SIGSEGV: stale pointer after `mremap(MREMAP_MAYMOVE)` in `btree_page_insert`
- Fixed critical SIGSEGV: dangling page chain pointer after `munmap` in `slb0_dispose`
- Test coverage: 35 tests, 100% passing, valgrind clean
- New test suites: `test_realloc.c` (REA-01–REA-08), `test_page_release.c` (PRL-01–PRL-05), `test_skip_list_correctness.c` (SLC-01–SLC-03)

## [0.2.2] - 2026-03-15

**Arena System + Dynamic NodePool Growth**

- Added user arena system: 14 concurrent user arenas (scope_id 2–15)
- Arena API: `Allocator.create_arena(name, policy)`, `Allocator.dispose_arena(scope)`
- Dynamic NodePool growth via `mremap` — no more fixed-size metadata limit
- Replaces frame-based model (v0.2.1) with simpler O(1) bump-allocation arenas
- Test coverage: 31 tests, 100% passing, 0 bytes leaked

## [0.2.1] - 2026-03-08

**Frame Support**

- Frame support for SLB0: chunked bump allocators for bulk deallocation
- 17/17 frame tests passing, valgrind clean

## [0.2.0] - 2026-02-12

**MTIS Foundation**

- Complete architectural overhaul: pool allocator → Multi-Tiered Indexing Schema (MTIS)
- Two-tier hierarchical indexing: Skip List (PageList) + per-page B-trees
- O(log n) allocation and deallocation; zero malloc/free in core allocator
- 24-byte cache-aligned B-tree nodes; dynamic NodePool starting at 2KB
- Register machine model for minimal ABI overhead
