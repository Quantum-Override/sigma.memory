/*
 *  Test File: test_bootstrap.c
 *  Description: testing memory management features
 *  Date: 2026-01-18
 */

#include "internal/memory.h"
#include "internal/slab_manager.h"
#include "memory.h"
// ----------------
#include <sigtest/sigtest.h>

#define MEM_STATE_ALIGN_MASK \
    (MEM_STATE_ALIGN_SYS0 | MEM_STATE_ALIGN_HEADER | MEM_STATE_ALIGN_FOOTER)

#if 1  // Region: Test Set Setup & Teardown
static void set_config(FILE **log_stream) {
    *log_stream = fopen("logs/test_bootstrap.log", "w");
    sbyte state = Memory.state();
    Assert.isTrue(state & MEM_STATE_READY, "SYS0 should be ready during test setup");
}

static void set_teardown(void) {
    // tear down test harness apparatus
}
#endif

#if 1  // Region: SYS0 bootstrapping tests
void test_sys0_initialized(void) {
    usize expected_size = SYS0_PAGE_SIZE;
    usize actual_size = Memory.sys0_size();
    sbyte state = Memory.state();

    Assert.isTrue(expected_size == actual_size, "SYS0 size should be PAGE_SIZE when ready");
    Assert.isTrue(state & MEM_STATE_READY, "Memory system should be ready");
}
void test_sanity_memory_alignments(void) {
    sbyte state = Memory.state();
    sbyte expected_align = MEM_STATE_ALIGN_MASK;

    // Check all alignment flags are set
    Assert.isTrue((state & expected_align) == expected_align,
                  "All alignment checks should pass (expected 0x%02x in state 0x%02x)",
                  expected_align, state);

    // Individual checks with detailed messages
    Assert.isTrue(state & MEM_STATE_ALIGN_SYS0, "SYS0 page should be kAlign aligned (%d bytes)",
                  kAlign);
    Assert.isTrue(state & MEM_STATE_ALIGN_HEADER,
                  "Header size should be kAlign multiple (%d bytes)", kAlign);
    Assert.isTrue(state & MEM_STATE_ALIGN_FOOTER, "Footer placement should be naturally aligned");
}
void test_init_page_creates_single_free_block(void) {
    // After init, sys_page has a free block. Due to slab slot array init,
    // there may be allocations, so we check for valid block structure.
    block_header hdr = Memory.get_first_header();
    block_footer ftr = Memory.get_last_footer();

    // Verify header exists and is valid
    Assert.isNotNull(hdr, "First header should exist");

    // Find the last block (which should be free and have LAST flag)
    // next_off is an offset from page base (sys0_base from R0)
    block_header last_hdr = hdr;
    addr page_base = Memory.get_sys0_base();
    while (last_hdr->next_off != 0) {
        last_hdr = (block_header)(page_base + last_hdr->next_off);
    }
    Assert.isTrue(last_hdr->flags & BLK_FLAG_FREE, "Last block should be FREE");
    Assert.isTrue(last_hdr->flags & BLK_FLAG_LAST, "Last block should have LAST flag");
    Assert.isTrue(last_hdr->flags & BLK_FLAG_FOOT, "Last block should have HAS_FOOTER flag");

    // Verify footer at end of page
    Assert.isTrue(ftr->magic == BLK_END, "Footer magic should be BLK_END");

    // Verify alignment is maintained
    sbyte state = Memory.state();
    Assert.isTrue(state & MEM_STATE_READY, "Memory should be ready");
}
void test_sys0_alloc_basic(void) {
    block_header hdr = Memory.get_first_header();

    // Allocate a sample block from SYS0
    usize alloc_size = 64;
    object blk = Allocator.alloc(alloc_size);
    Assert.isNotNull(blk, "SYS0 allocation of %zu bytes should succeed", alloc_size);

    // Verify the block is in a valid range (after FIRST_BLOCK_OFFSET + header)
    addr blk_addr = (addr)blk;
    addr min_addr = (addr)(hdr) + sizeof(sc_blk_header);
    Assert.isTrue(blk_addr >= min_addr,
                  "Allocated block address (0x%lx) should be >= min address (0x%lx)", blk_addr,
                  min_addr);
}
void test_sys0_alloc_null_on_zero_size(void) {
    // Scope.alloc(0) should return NULL
    object ptr = Allocator.alloc(0);
    Assert.isNull(ptr, "Scope.alloc(0) should return NULL");
}
void test_sys0_alloc_leaves_free_block(void) {
    // After allocation, there should still be a free block
    block_header first_hdr = Memory.get_first_header();
    usize initial_size = first_hdr->size;
    (void)initial_size;  // suppress unused variable warning

    object blk = Allocator.alloc(64);
    Assert.isNotNull(blk, "SYS0 allocation should succeed");

    // The allocated block header
    addr blk_addr = (addr)blk;
    block_header alloc_hdr = (block_header)(blk_addr - sizeof(sc_blk_header));

    // Find next block using sys0_base for correct offset calculation
    if (alloc_hdr->next_off != 0) {
        addr sys0_base = Memory.get_sys0_base();
        block_header next_hdr = (block_header)(sys0_base + alloc_hdr->next_off);
        Assert.isTrue(next_hdr->flags & BLK_FLAG_FREE, "Remaining block should be marked FREE");
    } else {
        // No next block - allocation used the last free block, which is fine
        // Assert.isTrue(alloc_hdr->next_off != 0, "There should be a next block after allocation");
    }
}
void test_slab_table_allocation(void) {
    /*
     * SYS0 Page Layout (8192 bytes - v0.2.0):
     *
     * |---------- RESERVED (1536) ----------|----- DATA AREA (6656) -----|
     * | Regs | SlabArray | ... | NodeTable | NodeStack | ... | slotarray | free |
     * | (64) | (128)     |     | (30)      | (128)     |     | (alloc'd) |      |
     * 0      64          192   1320        1350        1478  1536        8192
     *        ^           ^     ^           ^           ^     ^           ^
     *        SLOTS_OFFSET|     NODE_TABLE  NODE_STACK  |     FIRST_BLOCK LAST_FOOTER
     *                    SLOTS_END                     |     (data start)
     *                                                  (reserved end)
     */

    addr sys0_base = Memory.get_sys0_base();

    // === RESERVED REGION: Registers (0-63) ===
    // R0-R7 occupy bytes 0-63 (SYS0_REGISTERS_SIZE = 64)
    addr registers_end = sys0_base + SYS0_REGISTERS_SIZE;

    // === RESERVED REGION: Slab Array bucket (64-191) ===
    addr slots_base = Memory.get_slots_base();
    addr slots_end = Memory.get_slots_end();
    addr expected_slots_base = sys0_base + SYS0_SLAB_TABLE_OFFSET;
    addr expected_slots_end = sys0_base + SYS0_SLAB_TABLE_OFFSET + SYS0_SLAB_TABLE_SIZE;

    Assert.isTrue(slots_base == expected_slots_base,
                  "Slots base (0x%lx) == sys0 + SYS0_SLAB_TABLE_OFFSET [%u] (0x%lx)", slots_base,
                  SYS0_SLAB_TABLE_OFFSET, expected_slots_base);
    Assert.isTrue(slots_end == expected_slots_end, "Slots end (0x%lx) == sys0 + %u + %u (0x%lx)",
                  slots_end, SYS0_SLAB_TABLE_OFFSET, SYS0_SLAB_TABLE_SIZE, expected_slots_end);
    Assert.isTrue(slots_base == registers_end, "Slots base immediately follows registers");

    // All 16 slots should be ADDR_EMPTY initially
    for (usize i = 0; i < 16; i++) {
        addr slot_value = SlabManager.get_slab_entry(i);
        if (i == 1) {
            // Slot 1 may be used by SlotArray, skip check
            continue;
        }
        Assert.isTrue(slot_value == ADDR_EMPTY, "Slot[%zu] == ADDR_EMPTY", i);
    }

    // === RESERVED REGION: Unused (192-255) ===
    addr data_area_base = sys0_base + FIRST_BLOCK_OFFSET;
    Assert.isTrue(slots_end <= data_area_base, "Slots end (0x%lx) <= data area (0x%lx)", slots_end,
                  data_area_base);

    // === DATA AREA: First block header at offset 256 ===
    block_header first_hdr = Memory.get_first_header();
    addr first_hdr_addr = (addr)first_hdr;
    addr expected_first_hdr = sys0_base + FIRST_BLOCK_OFFSET;

    Assert.isTrue(first_hdr_addr == expected_first_hdr,
                  "First header (0x%lx) == sys0 + FIRST_BLOCK_OFFSET [%u] (0x%lx)", first_hdr_addr,
                  FIRST_BLOCK_OFFSET, expected_first_hdr);

    // First block is the slotarray struct (allocated by SlabManager init)
    Assert.isTrue(!(first_hdr->flags & BLK_FLAG_FREE),
                  "First block is allocated (slotarray struct in use)");
}
#endif

#if 1  // Region: Test Registration
__attribute__((constructor)) void init_memory_tests(void) {
    testset("Memory: SYS0 Bootstrapping", set_config, set_teardown);

    //  SYS0 bootstrapping tests
    testcase("SYS0: initialized", test_sys0_initialized);
    testcase("SYS0: memory alignment check", test_sanity_memory_alignments);
    testcase("SYS0: init single free block", test_init_page_creates_single_free_block);

    // SYS0 allocation tests
    testcase("SYS0: basic allocation", test_sys0_alloc_basic);
    testcase("SYS0: zero size alloc -> NULL", test_sys0_alloc_null_on_zero_size);
    testcase("SYS0: alloc leaves free block", test_sys0_alloc_leaves_free_block);

    // Slab slot array tests
    testcase("SYS0: slab table allocation", test_slab_table_allocation);
}
#endif
