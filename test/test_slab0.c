/*
 *  Test File: test_slab0.c
 *  Description: SLB0 user-space memory system tests
 *  Date: 2026-01-20
 */

#include "internal/memory.h"
#include "internal/slab_manager.h"
#include "memory.h"
// ----------------
#include <sigtest/sigtest.h>

#if 1  // Region: Test Set Setup & Teardown
static void set_config(FILE **log_stream) {
    *log_stream = fopen("logs/test_slab0.log", "w");
}

static void set_teardown(void) {
    // tear down test harness apparatus
}
#endif

#if 1  // Region: SLB0 Bootstrapping Tests
void test_slb0_scope_exists(void) {
    // SLB0 scope struct should be allocated from SYS0 data area
    // and accessible via a getter (Memory.get_slb0() or similar)
    // For now, verify current scope is not the SYS0 scope (scope_id != 0)
    scope current = (scope)Allocator.Scope.current();
    Assert.isNotNull(current, "Current scope should exist");
    Assert.isTrue(current->scope_id == 1, "SLB0 scope_id should be 1");
}

void test_slb0_has_dynamic_policy(void) {
    // SLB0 should have DYNAMIC policy for auto-growing page chains
    scope slb0 = (scope)Allocator.Scope.current();
    Assert.isNotNull(slb0, "SLB0 scope should exist");
    Assert.isTrue(slb0->policy == SCOPE_POLICY_DYNAMIC,
                  "SLB0 policy should be DYNAMIC (%d), got %d", SCOPE_POLICY_DYNAMIC, slb0->policy);
}

void test_slb0_has_secure_flag_by_default(void) {
    // SLB0 should have SECURE flag set by default
    scope slb0 = (scope)Allocator.Scope.current();
    Assert.isNotNull(slb0, "SLB0 scope should exist");
    Assert.isTrue(slb0->flags & SCOPE_FLAG_SECURE, "SLB0 should have SECURE flag set");
}

void test_slb0_has_16_pages_initial(void) {
    // SLB0 should have 16 pages allocated initially
    scope slb0 = (scope)Allocator.Scope.current();
    Assert.isNotNull(slb0, "SLB0 scope should exist");
    Assert.isTrue(slb0->page_count == 16, "SLB0 should have 16 pages, got %zu", slb0->page_count);
}

void test_slb0_page_chain_initialized(void) {
    // All 16 pages should be linked via next_page_off
    scope slb0 = (scope)Allocator.Scope.current();
    Assert.isNotNull(slb0, "SLB0 scope should exist");
    Assert.isTrue(slb0->first_page_off != 0, "first_page_off should be set");

    // Walk the chain and count pages
    page_sentinel page = (page_sentinel)slb0->first_page_off;
    usize count = 1;
    while (page->next_page_off != 0) {
        page = (page_sentinel)page->next_page_off;
        count++;
        Assert.isTrue(count <= 16, "Page chain should not exceed 16 pages");
    }
    Assert.isTrue(count == 16, "Page chain should have 16 pages, counted %zu", count);
}

void test_slb0_first_page_sentinel_initialized(void) {
    // First page sentinel should have correct metadata
    scope slb0 = (scope)Allocator.Scope.current();
    Assert.isNotNull(slb0, "SLB0 scope should exist");

    page_sentinel page0 = (page_sentinel)slb0->first_page_off;
    Assert.isNotNull(page0, "Page 0 sentinel should exist");
    Assert.isTrue(page0->scope_id == 1, "Page 0 scope_id should be 1 (SLB0)");
    Assert.isTrue(page0->page_index == 0, "Page 0 index should be 0");
}

void test_slb0_bump_offset_starts_at_32(void) {
    // Fresh pages should have bump_offset at sizeof(sc_page_sentinel) = 32
    scope slb0 = (scope)Allocator.Scope.current();
    Assert.isNotNull(slb0, "SLB0 scope should exist");

    page_sentinel page0 = (page_sentinel)slb0->first_page_off;
    Assert.isNotNull(page0, "Page 0 sentinel should exist");
    Assert.isTrue(page0->bump_offset == sizeof(sc_page_sentinel),
                  "Bump offset should be %zu, got %lu", sizeof(sc_page_sentinel),
                  page0->bump_offset);
}

void test_slb0_slot1_has_page0_address(void) {
    // Slab slot[1] should hold the address of SLB0's page-0
    // (slot indices parallel scope_table indices: [0]=SYS0, [1]=SLB0, etc.)
    scope slb0 = (scope)Allocator.Scope.current();
    Assert.isNotNull(slb0, "SLB0 scope should exist");

    addr slot1_value = SlabManager.get_slab_slot(1);
    addr page0_addr = slb0->first_page_off;

    Assert.isTrue(slot1_value != ADDR_EMPTY, "Slot[1] should not be empty");
    Assert.isTrue(slot1_value == page0_addr, "Slot[1] (0x%lx) should equal page-0 address (0x%lx)",
                  slot1_value, page0_addr);
}

void test_slb0_is_current_scope(void) {
    // After init, SLB0 should be the current scope (in R7)
    scope current = (scope)Allocator.Scope.current();
    Assert.isNotNull(current, "Current scope should exist");
    Assert.isTrue(current->scope_id == 1, "Current scope should be SLB0 (id=1)");
    Assert.isTrue(current->policy == SCOPE_POLICY_DYNAMIC,
                  "Current scope should have DYNAMIC policy");
}

void test_slb0_initialized(void) {
    // Final check: MEM_STATE_USER_READY should be set after all SLB0 init
    sbyte state = Memory.state();

    // Verify bootstrap completed
    Assert.isTrue(state & MEM_STATE_READY, "Bootstrap memory system should be ready");

    // Verify user memory (SLB0) is ready - set only after full init
    Assert.isTrue(state & MEM_STATE_USER_READY, "User memory system (SLB0) should be ready");
}
#endif

#if 1  // Region: Allocation Tests
void test_slb0_alloc_basic(void) {
    // Basic allocation should succeed
    object ptr = Allocator.alloc(64);
    Assert.isNotNull(ptr, "Basic 64-byte allocation should succeed");

    // Verify alignment
    Assert.isTrue(((addr)ptr % kAlign) == 0, "Allocation should be kAlign-aligned");

    Allocator.dispose(ptr);
}

void test_slb0_alloc_minimum_size(void) {
    // Allocations smaller than SLB0_MIN_ALLOC should be rounded up
    object ptr = Allocator.alloc(1);
    Assert.isNotNull(ptr, "1-byte allocation should succeed (rounded to min)");

    Allocator.dispose(ptr);
}

void test_slb0_alloc_updates_bump_offset(void) {
    // Get initial bump offset
    scope slb0 = (scope)Allocator.Scope.current();
    page_sentinel page = (page_sentinel)slb0->current_page_off;
    addr initial_offset = page->bump_offset;

    // Allocate
    object ptr = Allocator.alloc(32);
    Assert.isNotNull(ptr, "Allocation should succeed");

    // Bump offset should have advanced
    Assert.isTrue(page->bump_offset > initial_offset,
                  "Bump offset should advance after allocation");

    Allocator.dispose(ptr);
}

void test_slb0_alloc_increments_alloc_count(void) {
    scope slb0 = (scope)Allocator.Scope.current();
    page_sentinel page = (page_sentinel)slb0->current_page_off;
    usize initial_count = page->alloc_count;

    object ptr = Allocator.alloc(64);
    Assert.isNotNull(ptr, "Allocation should succeed");
    Assert.isTrue(page->alloc_count == initial_count + 1,
                  "alloc_count should increment (got %zu, expected %zu)", page->alloc_count,
                  initial_count + 1);

    Allocator.dispose(ptr);
}

void test_slb0_alloc_multiple(void) {
    // Multiple allocations should succeed
    object ptrs[10];
    for (int i = 0; i < 10; i++) {
        ptrs[i] = Allocator.alloc(32);
        Assert.isNotNull(ptrs[i], "Allocation %d should succeed", i);
    }

    // Clean up
    for (int i = 0; i < 10; i++) {
        Allocator.dispose(ptrs[i]);
    }
}

void test_slb0_dispose_decrements_alloc_count(void) {
    scope slb0 = (scope)Allocator.Scope.current();
    page_sentinel page = (page_sentinel)slb0->current_page_off;

    // Keep one allocation to prevent page release
    object keeper = Allocator.alloc(32);
    Assert.isNotNull(keeper, "Keeper allocation should succeed");

    object ptr = Allocator.alloc(64);
    usize count_after_alloc = page->alloc_count;

    Allocator.dispose(ptr);

    Assert.isTrue(page->alloc_count == count_after_alloc - 1,
                  "alloc_count should decrement after dispose");

    Allocator.dispose(keeper);
}

void test_slb0_dispose_adds_to_free_list(void) {
    scope slb0 = (scope)Allocator.Scope.current();
    page_sentinel page = (page_sentinel)slb0->current_page_off;

    // Keep one allocation to prevent page release
    object keeper = Allocator.alloc(32);
    Assert.isNotNull(keeper, "Keeper allocation should succeed");

    object ptr = Allocator.alloc(64);
    addr ptr_addr = (addr)ptr;

    Allocator.dispose(ptr);

    // Free list head should now point to the disposed block
    Assert.isTrue(page->free_list_head == ptr_addr,
                  "free_list_head should point to disposed block");

    Allocator.dispose(keeper);
}

void test_slb0_alloc_reuses_free_block(void) {
    scope slb0 = (scope)Allocator.Scope.current();
    page_sentinel page = (page_sentinel)slb0->current_page_off;

    // Keep one allocation to prevent page release
    object keeper = Allocator.alloc(32);
    Assert.isNotNull(keeper, "Keeper allocation should succeed");

    // Allocate and dispose a block
    object ptr1 = Allocator.alloc(32);
    addr ptr1_addr = (addr)ptr1;
    Allocator.dispose(ptr1);

    // Verify it's in the free list
    Assert.isTrue(page->free_list_head == ptr1_addr, "Disposed block should be in free list");

    // Next allocation of same size should reuse the freed block
    object ptr2 = Allocator.alloc(16);  // SLB0_MIN_ALLOC or less
    Assert.isTrue((addr)ptr2 == ptr1_addr, "Should reuse freed block (got 0x%lx, expected 0x%lx)",
                  (addr)ptr2, ptr1_addr);

    Allocator.dispose(ptr2);
    Allocator.dispose(keeper);
}

void test_slb0_alloc_large(void) {
    // Large allocation that fits in a page
    object ptr = Allocator.alloc(2048);
    Assert.isNotNull(ptr, "2KB allocation should succeed");

    Allocator.dispose(ptr);
}

void test_slb0_page_release_on_empty(void) {
    // Get initial page count
    scope slb0 = (scope)Allocator.Scope.current();
    usize initial_page_count = slb0->page_count;

    // Fill first page to force overflow to second page
    // Page is 4096 bytes, sentinel is 32 bytes, so ~4064 usable
    object large = Allocator.alloc(4000);  // Nearly fills first page
    Assert.isNotNull(large, "Large allocation should succeed");

    // This should go to second page
    object ptr = Allocator.alloc(64);
    Assert.isNotNull(ptr, "Overflow allocation should succeed");

    // Get the page containing ptr
    page_sentinel page = (page_sentinel)slb0->current_page_off;
    usize page_alloc_count = page->alloc_count;
    Assert.isTrue(page_alloc_count == 1, "Second page should have 1 allocation");

    // Dispose the allocation on second page - this will release the page
    Allocator.dispose(ptr);

    // Page was released, so page_count should have decreased
    Assert.isTrue(slb0->page_count == initial_page_count - 1,
                  "Page count should decrease after empty page release (got %zu, expected %zu)",
                  slb0->page_count, initial_page_count - 1);

    // Clean up first page allocation
    Allocator.dispose(large);
}
#endif

#if 1  // Region: Test Registration
__attribute__((constructor)) void init_slab0_tests(void) {
    testset("SLB0: User Memory System", set_config, set_teardown);

    // SLB0 scope initialization (order follows init sequence)
    testcase("SLB0: scope exists", test_slb0_scope_exists);
    testcase("SLB0: has DYNAMIC policy", test_slb0_has_dynamic_policy);
    testcase("SLB0: has SECURE flag by default", test_slb0_has_secure_flag_by_default);

    // SLB0 page allocation
    testcase("SLB0: has 16 pages initial", test_slb0_has_16_pages_initial);
    testcase("SLB0: page chain initialized", test_slb0_page_chain_initialized);
    testcase("SLB0: first page sentinel initialized", test_slb0_first_page_sentinel_initialized);
    testcase("SLB0: bump offset starts at 32", test_slb0_bump_offset_starts_at_32);

    // SLB0 registration and activation
    testcase("SLB0: slot[1] has page-0 address", test_slb0_slot1_has_page0_address);
    testcase("SLB0: is current scope", test_slb0_is_current_scope);

    // Final state check
    testcase("SLB0: initialized (state ready)", test_slb0_initialized);

    // Allocation tests
    testcase("SLB0: alloc basic", test_slb0_alloc_basic);
    testcase("SLB0: alloc minimum size", test_slb0_alloc_minimum_size);
    testcase("SLB0: alloc updates bump offset", test_slb0_alloc_updates_bump_offset);
    testcase("SLB0: alloc increments alloc_count", test_slb0_alloc_increments_alloc_count);
    testcase("SLB0: alloc multiple", test_slb0_alloc_multiple);

    // Dispose tests
    testcase("SLB0: dispose decrements alloc_count", test_slb0_dispose_decrements_alloc_count);
    testcase("SLB0: dispose adds to free list", test_slb0_dispose_adds_to_free_list);
    testcase("SLB0: alloc reuses free block", test_slb0_alloc_reuses_free_block);
    testcase("SLB0: alloc large", test_slb0_alloc_large);
    testcase("SLB0: page release on empty", test_slb0_page_release_on_empty);
}
#endif
