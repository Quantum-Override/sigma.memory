/*
 * SigmaCore — sigma.memory
 * ----------------------------------------------
 * File: internal/memory.h
 * Description: Private implementation constants for sigma.memory v0.3.0.
 *              SYS0 geometry, memory-state flags, and SYS0-level diagnostic
 *              declarations.  Not installed; never included by consumers.
 */
#pragma once

#include <sigma.core/allocator.h>
#include <sigma.core/types.h>
#include "sigma.memory/memory.h"

// ── Alignment ──────────────────────────────────────────────────────────────
#define kAlign 16

// ── SYS0 page size ─────────────────────────────────────────────────────────
#define SYS0_PAGE_SIZE 8192  // 8 KB static bootstrap page

// ── SYS0 byte-offset layout ────────────────────────────────────────────────
//
//  [  0 –  63]  Register file R0–R7                      (64 bytes)
//  [ 64 – 191]  Reserved pad (was scope_table[16])       (128 bytes)
//  [192 – 455]  sc_ctrl_registry_s                       (<= 264 bytes)
//  [456 – 8191] SYS0-DAT: bootstrap first-fit allocator  (7736 bytes)
//
#define SYS0_REGISTERS_SIZE (8 * sizeof(addr))  // 64
#define SYS0_RESERVED_SIZE 128
#define SYS0_REGISTRY_SIZE 264

#define SYS0_REGISTERS_OFFSET 0
#define SYS0_RESERVED_OFFSET (SYS0_REGISTERS_OFFSET + SYS0_REGISTERS_SIZE)  // 64
#define SYS0_REGISTRY_OFFSET (SYS0_RESERVED_OFFSET + SYS0_RESERVED_SIZE)    // 192
#define SYS0_DAT_OFFSET (SYS0_REGISTRY_OFFSET + SYS0_REGISTRY_SIZE)         // 456

// POLICY_KERNEL defined in sigma.core/allocator.h (POLICY_KERNEL = 3).
// Redefined here as a fallback guard while the system header may lag.
#ifndef POLICY_KERNEL
#define POLICY_KERNEL 3
#endif

// ── MTIS structs (kernel controller internal metadata) ─────────────────────
// Ported from sigma.mem_0.2 node_pool.h; scoped per-controller (no global state).

// Skip-list constants
#define KNL_SKIP_LEVELS 4                   // 4-level skip list; handles 256+ pages
#define KNL_NODEPOOL_SIZE 16384u            // 16 KB nodepool mmap (page_node + sc_node arrays)
#define KNL_SLAB_SIZE (2u * 1024u * 1024u)  // 2 MB kernel arena (x86 huge-page boundary)
#define KNL_PAGE_SIZE 4096u                 // granularity for page tracking
#define KNL_MIN_ALLOC 16u                   // minimum allocation quantum

// Null sentinels
#define KNL_NODE_NULL ((uint16_t)0)
#define KNL_PAGE_NULL ((uint16_t)0)

// B-tree node — 24 bytes, cache-line friendly (3 per 64B line)
typedef struct knl_node_s {
    addr start;          // 8: allocation start address
    uint32_t length;     // 4: usable size in bytes
    uint16_t left_idx;   // 2: left child (KNL_NODE_NULL = none)
    uint16_t right_idx;  // 2: right child (KNL_NODE_NULL = none)
    uint16_t info;       // 2: flags (bit9 = FREE_FLAG)
    uint8_t _pad[6];     // 6: reserved / frame extensions
} knl_node_s;

#define KNL_FREE_FLAG 0x0200u  // bit 9 of info: block is free

// Page directory entry — skip-list node tracking one arena page
typedef struct knl_page_node_s {
    addr page_base;                     // 8: base address of tracked page
    uint16_t forward[KNL_SKIP_LEVELS];  // 8: skip-list forward pointers
    uint16_t btree_root;                // 2: root of this page's B-tree
    uint16_t block_count;               // 2: live B-tree entries
    uint16_t bump_offset;               // 2: bump pointer within page
    uint16_t alloc_count;               // 2: live allocation count
} knl_page_node_s;                      // Total: 24 bytes

// NodePool header — at the start of the 8 KB nodepool mmap
// page_nodes grow up from offset sizeof(knl_nodepool_hdr_s).
// knl_node_s entries grow down from the top.
typedef struct knl_nodepool_hdr_s {
    usize capacity;            // 8: total mmap size
    usize page_count;          // 8: pages in skip list
    usize page_alloc_off;      // 8: next free page_node slot (grows up)
    usize btree_alloc_off;     // 8: next free btree_node slot (grows down)
    uint16_t skip_head;        // 2: index of first page_node in skip list
    uint16_t btree_free_head;  // 2: recycled btree_node free list head
    uint16_t _reserved[6];     // 12: reserved
} knl_nodepool_hdr_s;          // Total: 40 bytes

// ── sc_kernel_ctrl_s ───────────────────────────────────────────────────────
// Baked into SYS0 DAT. Not in the registry. Never released.
// Backing: 2 MB mmap (KNL_SLAB_SIZE). Nodepool: 8 KB separate mmap.
// SLB0 delegates slb0_alloc/free/realloc to this controller.
typedef struct sc_kernel_ctrl_s {
    sc_ctrl_base_s base;           // MUST be first; policy = POLICY_KERNEL
    usize bump;                    // next-free offset in the 2 MB arena
    usize capacity;                // KNL_SLAB_SIZE
    knl_nodepool_hdr_s *nodepool;  // 8 KB mmap — page_node + knl_node_s arrays

    // vtable
    object (*alloc)(struct sc_kernel_ctrl_s *c, usize size);
    void (*free)(struct sc_kernel_ctrl_s *c, object ptr);
    object (*realloc)(struct sc_kernel_ctrl_s *c, object ptr, usize new_size);
} sc_kernel_ctrl_s;

// ── SYS0 DAT layout ───────────────────────────────────────────────────────
//   [DAT+0]            sc_reclaim_ctrl_s  — SLB0 (R7, POLICY_RECLAIM first-fit)
//   [DAT+SLB0_CTRL_SZ] sc_slab_s          — SLB0 slab descriptor
//   [DAT+SLB0_END]     sc_kernel_ctrl_s   — kernel controller (MTIS, not in registry)
//   [DAT+KNL_CTRL_OFF+KCTRL_SZ] sc_slab_s — kernel 2 MB slab descriptor
#define KNL_CTRL_OFF                                                                           \
    (((usize)sizeof(struct sc_reclaim_ctrl_s) + (usize)(kAlign - 1)) & ~(usize)(kAlign - 1)) + \
        (((usize)sizeof(sc_slab_s) + (usize)(kAlign - 1)) & ~(usize)(kAlign - 1))
#define KNL_CTRL_SZ (((usize)sizeof(sc_kernel_ctrl_s) + (usize)(kAlign - 1)) & ~(usize)(kAlign - 1))

// ── Memory state flags ─────────────────────────────────────────────────────
enum {
    MEM_STATE_SYS0_MAPPED = 1u << 0,
    MEM_STATE_BOOTSTRAP_COMPLETE = 1u << 1,
    MEM_STATE_SLB0_READY = 1u << 2,
    MEM_STATE_READY = 1u << 3,
    MEM_STATE_KERNEL_READY = 1u << 4,
};

// ── SYS0-level diagnostics (declared here; defined in memory.c) ────────────
addr memory_sys0_base(void);
usize memory_sys0_size(void);
uint8_t memory_state(void);
addr memory_r7(void);

// ── Trusted subsystem grant (defined in memory.c, used by module.c) ────────────
sc_trusted_cap_t *trusted_grant(const char *name, usize size, sc_alloc_policy policy);

// ── Module lifecycle hooks (called by src/module.c via sigma.core module system) ──
void init_memory_system(void);
void cleanup_memory_system(void);

// ── Test utility — Trusted subsystem reset ───────────────────────────────────────
// Unmaps trusted arenas and resets the slot counter.
// Does NOT touch SLB0. Safe to call mid-session (sigma.test allocations remain valid).
void memory_trusted_reset(void);
