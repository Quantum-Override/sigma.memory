/*
 * SigmaCore
 * Copyright (c) 2026 David Boarman (BadKraft) and contributors
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
 * File: sigma.memory/memory.h
 * Description: Sigma.Memory v0.3.0 public header.
 *              Full struct definitions for types whose opaque typedefs live in
 *              <sigma.core/allocator.h>.  Add Phase-by-phase as each controller
 *              type is implemented.
 */
#pragma once

#include <sigma.core/allocator.h>
#include <sigma.core/types.h>

// ── Max registered controllers ────────────────────────────────────────────
#define SC_MAX_CONTROLLERS 32

// ── sc_slab_s — raw mmap region, no embedded policy ───────────────────────
struct sc_slab_s {
    void *base;       // mmap base address
    usize size;       // total mapped bytes
    uint8_t slab_id;  // unique id assigned at acquire; 0 = unregistered
};

// ── sc_ctrl_base_s — common prefix for every controller struct ────────────
// Safe to cast any controller pointer to sc_ctrl_base_s * (base is first).
struct sc_ctrl_base_s {
    sc_alloc_policy policy;  // POLICY_BUMP or POLICY_RECLAIM
    slab backing;            // slab this controller drives
    usize struct_size;       // sizeof the concrete controller
    bool external;           // true → registered externally; release() won't slb0_free
    void (*shutdown)(struct sc_ctrl_base_s *ctrl);
};

// ── sc_ctrl_registry_s — embedded in SYS0 ────────────────────────────────
// entries[0] == SLB0 (set at bootstrap, never released).
// Slots 1–31 are user controllers.
typedef struct sc_ctrl_registry_s {
    sc_ctrl_base_s *entries[SC_MAX_CONTROLLERS];
    uint8_t count;
} sc_ctrl_registry_s;

// ── Phase 2: sc_bump_ctrl_s ───────────────────────────────────────────────
#define SC_FRAME_DEPTH_MAX 16

struct sc_bump_ctrl_s {
    sc_ctrl_base_s base;  // MUST be first

    usize cursor;    // next-free byte offset
    usize capacity;  // usable bytes (slab->size, aligned down)

    usize frame_stack[SC_FRAME_DEPTH_MAX];  // saved cursor positions
    uint8_t frame_depth;                    // current nesting depth

    // vtable
    object (*alloc)(struct sc_bump_ctrl_s *c, usize size);
    void (*reset)(struct sc_bump_ctrl_s *c, bool zero);
    frame (*frame_begin)(struct sc_bump_ctrl_s *c);
    void (*frame_end)(struct sc_bump_ctrl_s *c, frame f);
};

// ── Phase 3: sc_reclaim_ctrl_s ───────────────────────────────────────────
// Option-B first-fit free list.  Each allocation is preceded by a fixed-size
// block header embedded in the slab.  Free blocks reuse the header as a
// linked-list node.  Sequence tags enable frame_end batch reclaim.

#define RC_FLAG_FREE 0u  // block is on the free list
#define RC_FLAG_LIVE 1u  // block was returned to caller

// Header prepended to every allocation (16 bytes, kAlign-aligned).
typedef struct rc_block_hdr_s {
    usize size;      // usable bytes following this header (excludes header itself)
    uint32_t seq;    // frame sequence at alloc time (0 = pre-frame / SLB0 use)
    uint32_t flags;  // RC_FLAG_LIVE or RC_FLAG_FREE
    // When FREE: the 8 bytes immediately after this header hold a
    // struct rc_block_hdr_s * pointing to the next free block.
} rc_block_hdr_s;

struct sc_reclaim_ctrl_s {
    sc_ctrl_base_s base;  // MUST be first

    usize bump;      // next-free byte offset for bump expansion
    usize capacity;  // total usable bytes in backing slab

    rc_block_hdr_s *free_head;  // address-ordered singly-linked free list

    uint32_t seq;        // monotonic counter; incremented at frame_begin
    uint32_t frame_seq;  // seq value at the most recent frame_begin (0=none)

    // vtable
    object (*alloc)(struct sc_reclaim_ctrl_s *c, usize size);
    void (*free)(struct sc_reclaim_ctrl_s *c, object ptr);
    object (*realloc)(struct sc_reclaim_ctrl_s *c, object ptr, usize new_size);
    frame (*frame_begin)(struct sc_reclaim_ctrl_s *c);
    void (*frame_end)(struct sc_reclaim_ctrl_s *c, frame f);
};

// ── Test-accessible diagnostics ───────────────────────────────────────────
sc_ctrl_registry_s *memory_registry(void);
void slb_release_raw(slab s);                      // munmap only, no controller
typedef struct sc_kernel_ctrl_s sc_kernel_ctrl_s;  // full def in internal/memory.h
sc_kernel_ctrl_s *memory_kernel_ctrl(void);        // diagnostic: returns SYS0 kernel ctrl ptr

// ── FT-11: Trusted subsystem capability ───────────────────────────────────
// Returned by trusted_grant() and passed as ctx to SIGMA_ROLE_TRUSTED init().
// The trusted module stores this and routes all its allocations through alloc_use.

#define TRUSTED_SLOT_MIN 1u
#define TRUSTED_SLOT_MAX 6u
#define TRUSTED_SLAB_DEFAULT (256u * 1024u)  // 256 KB default if arena_size == 0

// ── FT-14: Trusted-app subsystem capability ───────────────────────────────
// Parallel pool for SIGMA_ROLE_TRUSTED_APP modules (app tier, not Ring1).
// Slots are numbered starting at TRUSTED_APP_SLOT_MIN, independent of R1–R6.
#define TRUSTED_APP_SLOT_MIN 1u
#define TRUSTED_APP_SLOT_MAX 8u

typedef struct sc_trusted_cap_s {
    uint8_t reg_slot;          // slot within its pool (1–6 for TRUSTED, 1–8 for TRUSTED_APP)
    slab arena;                // dedicated slab — never touches SLB0
    sc_ctrl_base_s *ctrl;      // reclaim or bump ctrl over the arena
    sc_alloc_use_t alloc_use;  // {alloc, release, resize} hooks routing through ctrl
} sc_trusted_cap_t;

// Diagnostic: retrieve a previously granted TRUSTED cap by register slot (1–6).
// Returns NULL if the slot is unassigned or the system is not ready.
sc_trusted_cap_t *memory_trusted_cap(uint8_t slot);

// Diagnostic: retrieve a previously granted TRUSTED_APP cap by app slot (1–8).
// Returns NULL if the slot is unassigned or the system is not ready.
sc_trusted_cap_t *memory_trusted_app_cap(uint8_t slot);
