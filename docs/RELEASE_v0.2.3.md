# Sigma.Memory v0.2.3 Release Notes

**Release Date:** March 9, 2026  
**Version:** 0.2.3-realloc  
**Status:** Beta Release (Realloc API + Dynamic Page Release + Skip List Correctness)  
**Test Coverage:** 35 tests, 100% passing, 0 bytes leaked (valgrind clean)

---

## Overview

Version 0.2.3 delivers three features across the SLB0 allocator and fixes two latent bugs discovered during test suite development. This is a patch-level release on top of v0.2.2; no API is removed and no ABI is broken.

---

## What's New

### 1. `Allocator.realloc(ptr, size)`

Standard realloc semantics with SLB0-native in-place shrink:

| Input | Behavior |
|-------|----------|
| `realloc(NULL, size)` | Equivalent to `alloc(size)` |
| `realloc(ptr, 0)` | Equivalent to `dispose(ptr)`, returns `NULL` |
| `realloc(ptr, smaller)` | Shrinks in-place; splits remainder into free node if above threshold |
| `realloc(ptr, larger)` | Allocates new block, copies data, disposes original |

```c
object buf = Allocator.alloc(64);
memcpy(buf, data, 64);

// Grow — data preserved
buf = Allocator.realloc(buf, 256);

// Shrink in-place — same pointer returned
buf = Allocator.realloc(buf, 32);

// Dispose via realloc
buf = Allocator.realloc(buf, 0);  // returns NULL
```

**Performance:**
- Shrink: O(log n) — in-place B-tree update, no copy
- Grow: O(log n) alloc + O(min(old,new)) memcpy + O(log n) dispose

---

### 2. SLB0 Dynamic Page Release

When all allocations on a dynamically-mmap'd SLB0 page are freed, the page is automatically returned to the OS via `munmap`.

**Conditions for release:**
- Page was created as an overflow page (beyond the initial 16-page contiguous block)
- B-tree tracking confirms the entire page data area is a single free block
- `page_count` is decremented; the page is unlinked from the page chain

**Effect:** Long-running processes with bursty allocation patterns no longer accumulate idle mapped pages indefinitely.

---

### 3. Skip List Correctness (Task 7 Regression Guard)

The skip list free-block search now correctly excludes fragmented pages from large allocation requests. Previously, a page with many small alternating free/used blocks could be returned for a large request even though no contiguous free region of sufficient size existed.

**Fix:** The skip list query path checks that the selected free block's contiguous length satisfies the requested size before committing. Fragmented pages are bypassed; a new page is mapped instead.

**Regression test:** `SLC-03` fills a page with 255×32-byte allocs, frees every other one (preventing coalescing), then requests a 1024-byte block — verifies the result pointer is NOT on the fragmented page.

---

## Bug Fixes

### BUG-1: Stale Pointer After `mremap(MREMAP_MAYMOVE)` in `btree_page_insert`

**File:** `src/node_pool.c` — `btree_page_insert`  
**Severity:** Critical (SIGSEGV)  
**Trigger:** NodePool growth during a B-tree insert while a raw `page_node *` pointer was live across the growth call.

`nodepool_alloc_btree_node` may internally call `mremap(MREMAP_MAYMOVE)` to grow the NodePool. This can relocate the entire mapping to a new virtual address, invalidating any raw pointer into it computed before the call.

**Fix:** Re-fetch `page = nodepool_get_page_node(scope_ptr, page_idx)` immediately after `nodepool_alloc_btree_node` returns; add NULL guard that frees the new node index on failure.

**Why it was dormant:** No existing test allocated enough objects to trigger NodePool growth within a single `btree_page_insert` scope. The page-release test suite was the first to do so.

---

### BUG-2: Page Chain Dangling Pointer After `munmap` in `slb0_dispose`

**File:** `src/memory.c` — `slb0_dispose`  
**Severity:** Critical (SIGSEGV)  
**Trigger:** After releasing a dynamic page via `munmap`, the predecessor page's `next_page_off` was left pointing at unmapped memory. The next `fallback_bump` allocation walk dereferenced the stale pointer.

**Fix:** Walk the page chain to locate the predecessor; update `prev->next_page_off = page->next_page_off` before calling `munmap`. Handles both first-page and mid-chain cases.

---

## Test Suites Added

### `test/unit/test_realloc.c` — 8 tests (REA-01 to REA-08)

| ID | Description |
|----|-------------|
| REA-01 | `NULL` ptr → allocates new block |
| REA-02 | `NULL` ptr + 0 → no-op, returns `NULL` |
| REA-03 | Non-NULL ptr + 0 → disposes, returns `NULL` |
| REA-04 | Shrink in-place: same pointer returned |
| REA-05 | Grow: data integrity preserved |
| REA-06 | Grow then shrink cycle |
| REA-07 | Remainder below split threshold: no split, no leak |
| REA-08 | Independent allocation chains: no cross-chain interference |

### `test/unit/test_page_release.c` — 5 tests (PRL-01 to PRL-05)

| ID | Description |
|----|-------------|
| PRL-01 | Initial page count is at least 16 |
| PRL-02 | Allocating beyond capacity creates a dynamic page |
| PRL-03 | Freeing all allocs on a dynamic page releases it (`page_count` drops) |
| PRL-04 | Initial-page allocs survive dynamic page churn |
| PRL-05 | Allocations succeed normally after a page release |

### `test/unit/test_skiplist_correctness.c` — 4 tests (SLC-01 to SLC-04)

| ID | Description |
|----|-------------|
| SLC-01 | B-tree free blocks reused for same-size re-allocs (no new pages) |
| SLC-02 | Coalesced free blocks serve larger allocs |
| SLC-03 | **Task 7 regression**: fragmented page skipped for large alloc |
| SLC-04 | Full fill → reverse free → re-alloc: no new pages needed |

---

## Files Changed

### Source
- `src/node_pool.c` — Bug #1 fix: re-fetch `page_node *` after `nodepool_alloc_btree_node`
- `src/memory.c` — Bug #2 fix: unlink page from chain before `munmap`; `slb0_realloc` implementation

### Headers
- `include/memory.h` — `realloc` function pointer added to `sc_allocator_i`
- `include/internal/memory.h` — internal realloc forward declaration
- `include/internal/node_pool.h` — no functional changes

### Tests (new)
- `test/unit/test_realloc.c`
- `test/unit/test_page_release.c`
- `test/unit/test_skiplist_correctness.c`

### Documentation
- `docs/ROADMAP.md` — version bumped to 0.2.3-realloc, v0.2.3 section added
- `docs/USERS_GUIDE.md` — `Allocator.realloc` added to API table
- `docs/MEMORY_REFERENCE.md` — `Allocator.realloc` added to API quick-reference
- `docs/MEMORY_DESIGN.md` — Chapter 9 added; status footer updated
- `docs/RELEASE_v0.2.3.md` — this file

### Package
- `package/include/` — synced from `include/`
- `package/sigma.memory.o` — rebuilt

---

## Upgrade Notes

**From v0.2.2:** Drop-in compatible. `Allocator.realloc` is additive. No existing API changed. Recompile against the new headers.

**ABI:** The `sc_allocator_i` struct has a new `realloc` function pointer. Any code that initializes this struct by position (not by name) will break — use designated initializers.

---

## Known Issues

- Pre-existing `registers_get`/`registers_set`: `-Wtype-limits` warning on `index < 0` guard (signed comparison on unsigned type). Cosmetic — does not affect correctness.

---

## Next

See [ROADMAP.md](ROADMAP.md) for planned v0.2.4 work.
