/*
 * Sigma.Core
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
 * File: sigma.core/allocator.h
 * Description: Allocator interface for SigmaCore v0.3.0.
 *              Defines sc_allocator_i and the opaque types it traffics in.
 *              Implemented by sigma.memory (full slab+controller model) or
 *              sigma.system.alloc (malloc-backed stub; acquire/release/
 *              create_bump/create_reclaim return NULL).
 *
 *              The full struct definitions for slab, sc_ctrl_base_s,
 *              sc_bump_ctrl_s, sc_reclaim_ctrl_s are in sigma.memory/memory.h.
 *              sigma.core knows only the pointer typedefs and the interface.
 */
#pragma once

#include <sigma.core/types.h>

// ── Allocation policy ──────────────────────────────────────────────────────
typedef enum {
    POLICY_BUMP = 1,     // pure cursor, no individual free
    POLICY_RECLAIM = 2,  // first-fit free list, individual free / coalesce
    POLICY_KERNEL = 3,   // MTIS (skip-list PageList + B-tree NodePool); SLB0 / Ring0 use only
} sc_alloc_policy;

// ── Opaque frame handle ────────────────────────────────────────────────────
// bump_allocator  frames: stores saved cursor offset
// reclaim_allocator frames: stores sequence tag
// FRAME_NULL is the sentinel for "no active frame" or "frame_begin failed"
typedef usize frame;
#define FRAME_NULL ((frame)0)

// ── Forward typedefs — full structs defined in sigma.memory/memory.h ───────
typedef struct sc_slab_s sc_slab_s;
typedef sc_slab_s *slab;  // raw mmap region

typedef struct sc_ctrl_base_s sc_ctrl_base_s;         // common controller header
typedef struct sc_bump_ctrl_s *bump_allocator;        // cursor-only controller
typedef struct sc_reclaim_ctrl_s *reclaim_allocator;  // MTIS controller

// ── Lightweight allocator hook ─────────────────────────────────────────────
// Used by Text interfaces (String, StringBuilder) and SIGMA_ROLE_ISOLATED modules
// as an optional allocator configuration.  NULL fields (or a NULL pointer to
// this struct) fall back to system malloc / free / realloc.
typedef struct sc_alloc_use_s {
    void *(*alloc)(usize size);
    void (*release)(void *ptr);
    void *(*resize)(void *ptr, usize size);
} sc_alloc_use_t;

// ── Custom controller factory function type ────────────────────────────────
// Factory receives the mmap-backed slab; must return a sc_ctrl_base_s *
// (i.e. the first field of the concrete controller struct must be sc_ctrl_base_s base).
// Used by create_custom().  The returned pointer is registered and later freed
// via slb0_free() on release(), unless register_ctrl() is used externally.
typedef sc_ctrl_base_s *(*ctrl_factory_fn)(slab s);

// ── Allocator facade interface ─────────────────────────────────────────────
typedef struct sc_allocator_i {
    // Raw slab lifecycle
    slab (*acquire)(usize size);            // mmap + slab_id assign
    void (*release)(sc_ctrl_base_s *ctrl);  // shutdown + deregister + slb0_free (if not external)

    // Typed controller constructors — slab created internally, registered in registry
    bump_allocator (*create_bump)(usize size);
    reclaim_allocator (*create_reclaim)(usize size);

    // Top-level facade — always dispatches to SLB0 reclaim ctrl (R7 fixed)
    object (*alloc)(usize size);
    void (*free)(object ptr);
    object (*realloc)(object ptr, usize new_size);

    // Phase 4: custom factory + external registration (appended to preserve ABI)
    sc_ctrl_base_s *(*create_custom)(usize size, ctrl_factory_fn factory);
    void (*register_ctrl)(sc_ctrl_base_s *ctrl);

    // Diagnostics
    bool (*is_ready)(void);  // true iff init_memory_system() completed successfully
} sc_allocator_i;

extern const sc_allocator_i Allocator;
