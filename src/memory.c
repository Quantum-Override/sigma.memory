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
 * File: memory.c
 * Description: sigma.memory v0.3.0 — Phase 1.
 *              SYS0 bootstrap, SLB0 controller, Allocator facade,
 *              raw slab lifecycle.
 */
#define _GNU_SOURCE
#include "memory.h"
#include "internal/memory.h"

#include <string.h>
#include <sys/mman.h>

// ── Forward declarations — API ─────────────────────────────────────────────
void init_memory_system(void);
void cleanup_memory_system(void);
static bool allocator_is_ready(void);
object slb0_alloc(usize size);
void slb0_free(object ptr);
object slb0_realloc(object ptr, usize new_size);
static slab allocator_acquire(usize size);
static void allocator_release(sc_ctrl_base_s *ctrl);
static bump_allocator allocator_create_bump(usize size);
static reclaim_allocator allocator_create_reclaim(usize size);
static sc_ctrl_base_s *allocator_create_custom(usize size, ctrl_factory_fn factory);
static void allocator_register_ctrl(sc_ctrl_base_s *ctrl);

// ── Forward declarations — helpers ────────────────────────────────────────
static void bootstrap_sys0(void);
static void bootstrap_slb0(void);
static void bootstrap_kernel(void);

// kernel controller helpers (MTIS)
static object kernel_alloc(sc_kernel_ctrl_s *c, usize size);
static void kernel_free(sc_kernel_ctrl_s *c, object ptr);
static object kernel_realloc(sc_kernel_ctrl_s *c, object ptr, usize new_size);
// nodepool helpers
static bool knl_nodepool_grow(sc_kernel_ctrl_s *c);
static knl_page_node_s *knl_page_alloc(sc_kernel_ctrl_s *c);
static knl_node_s *knl_btree_alloc(sc_kernel_ctrl_s *c);
static void knl_btree_free_node(sc_kernel_ctrl_s *c, uint16_t idx);
static uint16_t knl_page_index(sc_kernel_ctrl_s *c, knl_page_node_s *pn);
static knl_node_s *knl_node_at(sc_kernel_ctrl_s *c, uint16_t idx);
static knl_page_node_s *knl_page_at(sc_kernel_ctrl_s *c, uint16_t idx);
// skip-list helpers
static int knl_skip_random_level(void);
static uint16_t knl_skip_find_for_size(sc_kernel_ctrl_s *c, usize size);
static uint16_t knl_skip_find_containing(sc_kernel_ctrl_s *c, addr address);
static void knl_skip_insert(sc_kernel_ctrl_s *c, knl_page_node_s *pn);
// NOTE(artifact): knl_skip_remove was written for the original SLB0 page-release path
// (grow from 16 pages @ 4 KB, release back to min when alloc count hits 0). The kernel
// controller moved to a single fixed 2 MB arena — pages are formatting within that
// arena, not individually mapped. Retained as a reference if per-page release is
// ever revisited (e.g., configurable arena size with runtime shrink).
// static void knl_skip_remove(sc_kernel_ctrl_s *c, uint16_t pidx);
// B-tree helpers
static uint16_t knl_btree_insert(sc_kernel_ctrl_s *c, uint16_t pidx, addr start, usize len);
static uint16_t knl_btree_page_search(sc_kernel_ctrl_s *c, uint16_t pidx, addr start);
static void knl_btree_find_best_recursive(sc_kernel_ctrl_s *c, uint16_t cur, usize need,
                                          uint16_t *best, uint32_t *best_sz);
static uint16_t knl_btree_find_free(sc_kernel_ctrl_s *c, uint16_t pidx, usize need);
static uint16_t knl_btree_find_min(sc_kernel_ctrl_s *c, uint16_t idx);
static uint16_t knl_btree_find_max(sc_kernel_ctrl_s *c, uint16_t idx);
static uint16_t knl_btree_find_predecessor(sc_kernel_ctrl_s *c, uint16_t root, uint16_t target);
static uint16_t knl_btree_find_successor(sc_kernel_ctrl_s *c, uint16_t root, uint16_t target);
static uint16_t knl_btree_delete_recursive(sc_kernel_ctrl_s *c, uint16_t cur, uint16_t del,
                                           bool *deleted);
static void knl_btree_delete(sc_kernel_ctrl_s *c, uint16_t pidx, uint16_t nidx);
static void knl_btree_coalesce(sc_kernel_ctrl_s *c, uint16_t pidx, uint16_t nidx);
// page lifecycle
static uint16_t knl_ensure_page(sc_kernel_ctrl_s *c, usize need);
static object knl_bump_from_page(sc_kernel_ctrl_s *c, uint16_t pidx, usize need);

// bump controller helpers
static object bump_ctrl_alloc(struct sc_bump_ctrl_s *c, usize size);
static void bump_ctrl_reset(struct sc_bump_ctrl_s *c, bool zero);
static frame bump_ctrl_frame_begin(struct sc_bump_ctrl_s *c);
static void bump_ctrl_frame_end(struct sc_bump_ctrl_s *c, frame f);
static void bump_ctrl_shutdown(sc_ctrl_base_s *base);

// reclaim controller helpers
static object reclaim_ctrl_alloc(struct sc_reclaim_ctrl_s *c, usize size);
static void reclaim_ctrl_free(struct sc_reclaim_ctrl_s *c, object ptr);
static object reclaim_ctrl_realloc(struct sc_reclaim_ctrl_s *c, object ptr, usize new_size);
static frame reclaim_ctrl_frame_begin(struct sc_reclaim_ctrl_s *c);
static void reclaim_ctrl_frame_end(struct sc_reclaim_ctrl_s *c, frame f);
static void reclaim_ctrl_shutdown(sc_ctrl_base_s *base);
static object reclaim_alloc_from(struct sc_reclaim_ctrl_s *c, usize size, uint32_t seq);
static void reclaim_free_block(struct sc_reclaim_ctrl_s *c, rc_block_hdr_s *hdr);

// trusted subsystem helpers
sc_trusted_cap_t *trusted_grant(const char *name, usize size, sc_alloc_policy policy);
sc_trusted_cap_t *trusted_app_grant(const char *name, usize size, sc_alloc_policy policy);

// ── Private struct definitions ────────────────────────────────────────────
typedef struct sc_registers_s {
    addr R[8];  // R0–R7; R7 is fixed to SLB0 ctrl at bootstrap, never changes
} sc_registers_s;

// sc_reclaim_ctrl_s is fully defined in sigma.memory/memory.h (Phase 3).

// ── SYS0-DAT layout (from SYS0_DAT_OFFSET) ───────────────────────────────
//   [+0]              sc_reclaim_ctrl_s  — SLB0 ctrl (R7, first-fit)
//   [+SLB0_CTRL_SZ]   sc_slab_s          — SLB0 64 KB slab descriptor
//   [+KNL_CTRL_OFF]   sc_kernel_ctrl_s   — kernel ctrl (MTIS, not in registry)
//   [+KNL_CTRL_OFF
//    +KNL_CTRL_SZ]    sc_slab_s          — kernel 2 MB slab descriptor
//
#define SLB0_SIZE 65536u
#define SLB0_CTRL_SZ \
    (((usize)sizeof(struct sc_reclaim_ctrl_s) + (usize)(kAlign - 1)) & ~(usize)(kAlign - 1))
#define SLB0_SLAB_SZ (((usize)sizeof(sc_slab_s) + (usize)(kAlign - 1)) & ~(usize)(kAlign - 1))

// ── File-static state ──────────────────────────────────────────────────────
static addr s_sys0_base = ADDR_EMPTY;
static uint8_t s_state = 0;
static uint8_t s_next_slab_id = 2u;  // 1 = SLB0, 2+ = user slabs

// Trusted subsystem caps (R1–R6); index = slot - 1
static uint8_t s_next_trusted_slot = TRUSTED_SLOT_MIN;
static sc_trusted_cap_t *s_trusted_caps[TRUSTED_SLOT_MAX];  // [0]=R1 .. [5]=R6

// Trusted-app caps (FT-14); parallel pool, independent of R1–R6
static uint8_t s_next_trusted_app_slot = TRUSTED_APP_SLOT_MIN;
static sc_trusted_cap_t *s_trusted_app_caps[TRUSTED_APP_SLOT_MAX];  // [0]=A1 .. [7]=A8

// ── SYS0 region accessors ─────────────────────────────────────────────────
static inline sc_registers_s *sys0_regs(void) {
    return (sc_registers_s *)(s_sys0_base + SYS0_REGISTERS_OFFSET);
}

static inline sc_ctrl_registry_s *sys0_registry_ptr(void) {
    return (sc_ctrl_registry_s *)(s_sys0_base + SYS0_REGISTRY_OFFSET);
}

static inline struct sc_reclaim_ctrl_s *sys0_slb0_ctrl(void) {
    return (struct sc_reclaim_ctrl_s *)(s_sys0_base + SYS0_DAT_OFFSET);
}

static inline sc_slab_s *sys0_slb0_slab(void) {
    return (sc_slab_s *)(s_sys0_base + SYS0_DAT_OFFSET + SLB0_CTRL_SZ);
}

static inline sc_kernel_ctrl_s *sys0_kernel_ctrl(void) {
    return (sc_kernel_ctrl_s *)(s_sys0_base + SYS0_DAT_OFFSET + KNL_CTRL_OFF);
}

static inline sc_slab_s *sys0_kernel_slab(void) {
    return (sc_slab_s *)(s_sys0_base + SYS0_DAT_OFFSET + KNL_CTRL_OFF + KNL_CTRL_SZ);
}

// ── SLB0 — delegates to kernel controller (MTIS) ──────────────────────────
// Exported for weak linkage: sigma.core's Application.get_allocator() falls back to these
// symbols when no custom allocator is configured via Application.set_allocator().
object slb0_alloc(usize size) {
    sc_kernel_ctrl_s *k = sys0_kernel_ctrl();
    return k->alloc(k, size);
}

void slb0_free(object ptr) {
    sc_kernel_ctrl_s *k = sys0_kernel_ctrl();
    k->free(k, ptr);
}

object slb0_realloc(object ptr, usize new_size) {
    sc_kernel_ctrl_s *k = sys0_kernel_ctrl();
    return k->realloc(k, ptr, new_size);
}

// ── Allocator facade ──────────────────────────────────────────────────────
static slab allocator_acquire(usize size) {
    void *mem = NULL;
    slab s = NULL;

    if (size == 0) goto exit;

    mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (mem == MAP_FAILED) goto exit;

    s = slb0_alloc(sizeof(sc_slab_s));
    if (!s) goto cleanup;

    s->base = mem;
    s->size = size;
    s->slab_id = s_next_slab_id++;
    goto exit;

cleanup:
    munmap(mem, size);
exit:
    return s;
}

static void allocator_release(sc_ctrl_base_s *ctrl) {
    if (!ctrl) return;
    if (ctrl->shutdown) ctrl->shutdown(ctrl);  // munmaps the backing mmap region

    // Deregister from registry
    sc_ctrl_registry_s *reg = sys0_registry_ptr();
    for (uint8_t i = 1; i < SC_MAX_CONTROLLERS; i++) {
        if (reg->entries[i] == ctrl) {
            reg->entries[i] = NULL;
            if (i == reg->count - 1) reg->count--;
            break;
        }
    }

    // Only free SLB0-allocated structs for internally-created controllers.
    // shutdown() already munmap'd the backing mmap region; we only slb0_free the descriptors.
    if (!ctrl->external) {
        slb0_free(ctrl->backing);
        slb0_free(ctrl);
    }
}

// ── Registry helper ─────────────────────────────────────────────────────────
// Returns registry index on success, 0 on failure (slot 0 is always SLB0).
static uint8_t registry_insert(sc_ctrl_base_s *ctrl) {
    sc_ctrl_registry_s *reg = sys0_registry_ptr();
    // Search for a NULL slot in user range (1..SC_MAX_CONTROLLERS-1)
    for (uint8_t i = 1; i < SC_MAX_CONTROLLERS; i++) {
        if (!reg->entries[i]) {
            reg->entries[i] = ctrl;
            if (i >= reg->count) reg->count = i + 1;
            return i;
        }
    }
    return 0;  // full
}

static bump_allocator allocator_create_bump(usize size) {
    if (size == 0) return NULL;

    void *mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (mem == MAP_FAILED) return NULL;

    slab s = slb0_alloc(sizeof(sc_slab_s));
    if (!s) {
        munmap(mem, size);
        return NULL;
    }
    s->base = mem;
    s->size = size;
    s->slab_id = s_next_slab_id++;

    bump_allocator a = slb0_alloc(sizeof(struct sc_bump_ctrl_s));
    if (!a) {
        munmap(mem, size);
        slb0_free(s);
        return NULL;
    }

    a->base.policy = POLICY_BUMP;
    a->base.backing = s;
    a->base.struct_size = sizeof(struct sc_bump_ctrl_s);
    a->base.external = false;
    a->base.shutdown = bump_ctrl_shutdown;
    a->cursor = 0;
    a->capacity = size & ~(usize)(kAlign - 1);
    a->frame_depth = 0;
    a->alloc = bump_ctrl_alloc;
    a->reset = bump_ctrl_reset;
    a->frame_begin = bump_ctrl_frame_begin;
    a->frame_end = bump_ctrl_frame_end;

    if (!registry_insert((sc_ctrl_base_s *)a)) {
        slb0_free(a);
        munmap(mem, size);
        slb0_free(s);
        return NULL;
    }
    return a;
}

static reclaim_allocator allocator_create_reclaim(usize size) {
    if (size == 0) return NULL;

    void *mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (mem == MAP_FAILED) return NULL;

    slab s = slb0_alloc(sizeof(sc_slab_s));
    if (!s) {
        munmap(mem, size);
        return NULL;
    }
    s->base = mem;
    s->size = size;
    s->slab_id = s_next_slab_id++;

    reclaim_allocator r = slb0_alloc(sizeof(struct sc_reclaim_ctrl_s));
    if (!r) {
        munmap(mem, size);
        slb0_free(s);
        return NULL;
    }

    r->base.policy = POLICY_RECLAIM;
    r->base.backing = s;
    r->base.struct_size = sizeof(struct sc_reclaim_ctrl_s);
    r->base.external = false;
    r->base.shutdown = reclaim_ctrl_shutdown;
    r->bump = 0;
    r->capacity = size;
    r->free_head = NULL;
    r->seq = 0;
    r->frame_seq = 0;
    r->alloc = reclaim_ctrl_alloc;
    r->free = reclaim_ctrl_free;
    r->realloc = reclaim_ctrl_realloc;
    r->frame_begin = reclaim_ctrl_frame_begin;
    r->frame_end = reclaim_ctrl_frame_end;

    if (!registry_insert((sc_ctrl_base_s *)r)) {
        slb0_free(r);
        munmap(mem, size);
        slb0_free(s);
        return NULL;
    }
    return r;
}

static sc_ctrl_base_s *allocator_create_custom(usize size, ctrl_factory_fn factory) {
    if (size == 0 || !factory) return NULL;

    void *mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (mem == MAP_FAILED) return NULL;

    slab s = slb0_alloc(sizeof(sc_slab_s));
    if (!s) {
        munmap(mem, size);
        return NULL;
    }
    s->base = mem;
    s->size = size;
    s->slab_id = s_next_slab_id++;

    sc_ctrl_base_s *ctrl = factory(s);
    if (!ctrl) {
        munmap(mem, size);
        slb0_free(s);
        return NULL;
    }

    ctrl->external = false;

    if (!registry_insert(ctrl)) {
        if (ctrl->shutdown) ctrl->shutdown(ctrl);
        slb0_free(ctrl);
        munmap(mem, size);
        slb0_free(s);
        return NULL;
    }
    return ctrl;
}

static void allocator_register_ctrl(sc_ctrl_base_s *ctrl) {
    if (!ctrl) return;
    ctrl->external = true;
    registry_insert(ctrl);
}

// ── SYS0 bootstrap ────────────────────────────────────────────────────────
static void bootstrap_sys0(void) {
    void *base =
        mmap(NULL, SYS0_PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (base == MAP_FAILED) return;
    memset(base, 0, SYS0_PAGE_SIZE);
    s_sys0_base = (addr)base;
    s_state |= MEM_STATE_SYS0_MAPPED;
    sys0_regs()->R[0] = s_sys0_base;
}

static void bootstrap_slb0(void) {
    void *mem = mmap(NULL, SLB0_SIZE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (mem == MAP_FAILED) return;

    sc_slab_s *slb0_slab = sys0_slb0_slab();
    slb0_slab->base = mem;
    slb0_slab->size = SLB0_SIZE;
    slb0_slab->slab_id = 1u;

    struct sc_reclaim_ctrl_s *ctrl = sys0_slb0_ctrl();
    ctrl->base.policy = POLICY_RECLAIM;
    ctrl->base.backing = slb0_slab;
    ctrl->base.struct_size = sizeof(struct sc_reclaim_ctrl_s);
    ctrl->base.shutdown = NULL;  // SLB0 is never released via Allocator.release()
    ctrl->bump = 0;
    ctrl->capacity = SLB0_SIZE;
    ctrl->free_head = NULL;
    ctrl->seq = 0;
    ctrl->frame_seq = 0;
    ctrl->alloc = reclaim_ctrl_alloc;
    ctrl->free = reclaim_ctrl_free;
    ctrl->realloc = reclaim_ctrl_realloc;
    ctrl->frame_begin = reclaim_ctrl_frame_begin;
    ctrl->frame_end = reclaim_ctrl_frame_end;

    sc_ctrl_registry_s *reg = sys0_registry_ptr();
    reg->entries[0] = (sc_ctrl_base_s *)ctrl;
    reg->count = 1;

    sys0_regs()->R[7] = (addr)ctrl;
    s_state |= MEM_STATE_SLB0_READY;
}

static void bootstrap_kernel(void) {
    // mmap 2 MB kernel arena
    void *mem =
        mmap(NULL, KNL_SLAB_SIZE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (mem == MAP_FAILED) return;

    // mmap 8 KB nodepool region
    void *np_mem =
        mmap(NULL, KNL_NODEPOOL_SIZE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (np_mem == MAP_FAILED) {
        munmap(mem, KNL_SLAB_SIZE);
        return;
    }

    // Populate kernel slab descriptor (baked into SYS0 DAT)
    sc_slab_s *kslab = sys0_kernel_slab();
    kslab->base = mem;
    kslab->size = KNL_SLAB_SIZE;
    kslab->slab_id = 0u;  // out-of-band; not registered

    // Initialise nodepool header
    knl_nodepool_hdr_s *np = (knl_nodepool_hdr_s *)np_mem;
    np->capacity = KNL_NODEPOOL_SIZE;
    np->page_count = 0;
    np->page_alloc_off = sizeof(knl_nodepool_hdr_s);  // page_nodes grow up
    np->btree_alloc_off = KNL_NODEPOOL_SIZE;          // btree_nodes grow down
    np->skip_head = KNL_PAGE_NULL;
    np->btree_free_head = KNL_NODE_NULL;

    // Populate kernel ctrl (baked into SYS0 DAT)
    sc_kernel_ctrl_s *k = sys0_kernel_ctrl();
    k->base.policy = POLICY_KERNEL;
    k->base.backing = kslab;
    k->base.struct_size = sizeof(sc_kernel_ctrl_s);
    k->base.shutdown = NULL;  // kernel ctrl is never released
    k->bump = 0;
    k->capacity = KNL_SLAB_SIZE;
    k->nodepool = np;
    k->alloc = kernel_alloc;
    k->free = kernel_free;
    k->realloc = kernel_realloc;

    s_state |= MEM_STATE_KERNEL_READY;
}

void init_memory_system(void) {
    if (s_state & MEM_STATE_READY) return;  // idempotent
    bootstrap_sys0();
    if (!(s_state & MEM_STATE_SYS0_MAPPED)) return;
    bootstrap_kernel();
    if (!(s_state & MEM_STATE_KERNEL_READY)) return;
    bootstrap_slb0();
    if (!(s_state & MEM_STATE_SLB0_READY)) return;
    s_state |= MEM_STATE_BOOTSTRAP_COMPLETE | MEM_STATE_READY;
}

void cleanup_memory_system(void) {
    if (s_sys0_base == ADDR_EMPTY) return;
    // Shut down any live user controllers (entries[1..count-1]) — munmaps their backing slabs.
    // Do NOT slb0_free their structs; SLB0 itself is torn down below.
    sc_ctrl_registry_s *reg = sys0_registry_ptr();
    for (uint8_t i = 1; i < reg->count; i++) {
        sc_ctrl_base_s *ctrl = reg->entries[i];
        if (ctrl && ctrl->shutdown) ctrl->shutdown(ctrl);
    }
    // munmap SLB0 arena
    sc_slab_s *slb0 = sys0_slb0_slab();
    if (slb0->base) munmap(slb0->base, slb0->size);
    // munmap kernel nodepool + kernel arena
    sc_kernel_ctrl_s *k = sys0_kernel_ctrl();
    if (k->nodepool) munmap(k->nodepool, k->nodepool->capacity);
    sc_slab_s *kslab = sys0_kernel_slab();
    if (kslab->base) munmap(kslab->base, kslab->size);
    munmap((void *)s_sys0_base, SYS0_PAGE_SIZE);
    s_sys0_base = ADDR_EMPTY;
    s_state = 0;
    // Reset trusted subsystem state so init_memory_system() starts clean
    for (uint8_t i = 0; i < TRUSTED_SLOT_MAX; i++) {
        s_trusted_caps[i] = NULL;
    }
    s_next_trusted_slot = TRUSTED_SLOT_MIN;
    // Reset trusted-app subsystem state (FT-14)
    for (uint8_t i = 0; i < TRUSTED_APP_SLOT_MAX; i++) {
        s_trusted_app_caps[i] = NULL;
    }
    s_next_trusted_app_slot = TRUSTED_APP_SLOT_MIN;
}

// ── Diagnostics ───────────────────────────────────────────────────────────
addr memory_sys0_base(void) {
    return s_sys0_base;
}

usize memory_sys0_size(void) {
    return SYS0_PAGE_SIZE;
}

static bool allocator_is_ready(void) {
    return (s_state & MEM_STATE_READY) != 0;
}

uint8_t memory_state(void) {
    return s_state;
}

addr memory_r7(void) {
    if (s_sys0_base == ADDR_EMPTY) return ADDR_EMPTY;
    return sys0_regs()->R[7];
}

sc_ctrl_registry_s *memory_registry(void) {
    if (s_sys0_base == ADDR_EMPTY) return NULL;
    return sys0_registry_ptr();
}

sc_kernel_ctrl_s *memory_kernel_ctrl(void) {
    if (s_sys0_base == ADDR_EMPTY) return NULL;
    return sys0_kernel_ctrl();
}

sc_trusted_cap_t *memory_trusted_cap(uint8_t slot) {
    if (slot < TRUSTED_SLOT_MIN || slot > TRUSTED_SLOT_MAX) return NULL;
    return s_trusted_caps[slot - 1];
}

void memory_trusted_reset(void) {
    if (s_sys0_base == ADDR_EMPTY) return;
    for (uint8_t i = 0; i < TRUSTED_SLOT_MAX; i++) {
        sc_trusted_cap_t *cap = s_trusted_caps[i];
        if (cap) {
            // Unmap the dedicated arena (separate from SLB0 — safe to release)
            if (cap->arena && cap->arena->base) {
                munmap(cap->arena->base, cap->arena->size);
                cap->arena->base = NULL;
            }
            // Clear the register slot
            sys0_regs()->R[i + 1] = ADDR_EMPTY;
        }
        s_trusted_caps[i] = NULL;
    }
    s_next_trusted_slot = TRUSTED_SLOT_MIN;
}

sc_trusted_cap_t *memory_trusted_app_cap(uint8_t slot) {
    if (slot < TRUSTED_APP_SLOT_MIN || slot > TRUSTED_APP_SLOT_MAX) return NULL;
    return s_trusted_app_caps[slot - 1];
}

void memory_trusted_app_reset(void) {
    if (s_sys0_base == ADDR_EMPTY) return;
    for (uint8_t i = 0; i < TRUSTED_APP_SLOT_MAX; i++) {
        sc_trusted_cap_t *cap = s_trusted_app_caps[i];
        if (cap) {
            if (cap->arena && cap->arena->base) {
                munmap(cap->arena->base, cap->arena->size);
                cap->arena->base = NULL;
            }
        }
        s_trusted_app_caps[i] = NULL;
    }
    s_next_trusted_app_slot = TRUSTED_APP_SLOT_MIN;
}

void slb_release_raw(slab s) {
    if (!s || !s->base) return;
    munmap(s->base, s->size);
    s->base = NULL;
    slb0_free(s);
}

// ── Trusted subsystem grant ────────────────────────────────────────────
// Alloc/free/realloc hooks routed through a trusted module's reclaim ctrl.
// The ctrl pointer is captured in s_trusted_caps at grant time; the hooks
// look it up by the stored slot so each module gets its own controller path.
//
// We use thin top-level wrappers that forward to the stored cap's ctrl.
// One set of wrappers per slot avoids storing per-cap function pointer
// context in the hooks themselves.
//
// Limitation: only reclaim-policy trusted modules get wired alloc_use hooks;
// bump-policy trusted modules receive a zeroed alloc_use (they manage alloc
// themselves via cap->ctrl cast to bump_allocator).

static object trusted_slot_alloc(uint8_t slot, usize size) {
    sc_trusted_cap_t *cap = s_trusted_caps[slot - 1];
    if (!cap || !cap->ctrl) return NULL;
    struct sc_reclaim_ctrl_s *r = (struct sc_reclaim_ctrl_s *)cap->ctrl;
    return r->alloc(r, size);
}

static void trusted_slot_free(uint8_t slot, object ptr) {
    sc_trusted_cap_t *cap = s_trusted_caps[slot - 1];
    if (!cap || !cap->ctrl || !ptr) return;
    struct sc_reclaim_ctrl_s *r = (struct sc_reclaim_ctrl_s *)cap->ctrl;
    r->free(r, ptr);
}

static object trusted_slot_realloc(uint8_t slot, object ptr, usize new_size) {
    sc_trusted_cap_t *cap = s_trusted_caps[slot - 1];
    if (!cap || !cap->ctrl) return NULL;
    struct sc_reclaim_ctrl_s *r = (struct sc_reclaim_ctrl_s *)cap->ctrl;
    return r->realloc(r, ptr, new_size);
}

// Per-slot shims — one per R slot so each module's alloc_use hooks are distinct
// function pointers (required by sc_alloc_use_t which stores bare fn pointers
// with no context parameter).
#define TRUSTED_SHIM(N)                                       \
    static object trusted_r##N##_alloc(usize s) {             \
        return trusted_slot_alloc(N, s);                      \
    }                                                         \
    static void trusted_r##N##_free(object p) {               \
        trusted_slot_free(N, p);                              \
    }                                                         \
    static object trusted_r##N##_realloc(object p, usize s) { \
        return trusted_slot_realloc(N, p, s);                 \
    }

TRUSTED_SHIM(1)
TRUSTED_SHIM(2)
TRUSTED_SHIM(3)
TRUSTED_SHIM(4)
TRUSTED_SHIM(5)
TRUSTED_SHIM(6)

typedef object (*alloc_fn)(usize);
typedef void (*free_fn)(object);
typedef object (*realloc_fn)(object, usize);
typedef frame (*frame_begin_fn)(void);
typedef void (*frame_end_fn)(frame);

static const alloc_fn s_trusted_alloc_shims[TRUSTED_SLOT_MAX] = {
    trusted_r1_alloc, trusted_r2_alloc, trusted_r3_alloc,
    trusted_r4_alloc, trusted_r5_alloc, trusted_r6_alloc};
static const free_fn s_trusted_free_shims[TRUSTED_SLOT_MAX] = {trusted_r1_free, trusted_r2_free,
                                                               trusted_r3_free, trusted_r4_free,
                                                               trusted_r5_free, trusted_r6_free};
static const realloc_fn s_trusted_realloc_shims[TRUSTED_SLOT_MAX] = {
    trusted_r1_realloc, trusted_r2_realloc, trusted_r3_realloc,
    trusted_r4_realloc, trusted_r5_realloc, trusted_r6_realloc};

sc_trusted_cap_t *trusted_grant(const char *name, usize size, sc_alloc_policy policy) {
    (void)name;
    if (s_next_trusted_slot > TRUSTED_SLOT_MAX) return NULL;
    if (policy == POLICY_KERNEL) return NULL;  // reserved for Ring0
    // 0 (unset — old module descriptors compiled before arena_policy was added) → reclaim
    if (policy == 0) policy = POLICY_RECLAIM;

    usize arena_size = (size == 0) ? TRUSTED_SLAB_DEFAULT : size;
    uint8_t slot = s_next_trusted_slot;

    // Allocate the slab directly via mmap — NOT through SLB0 (trusted arenas are isolated)
    void *mem = mmap(NULL, arena_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (mem == MAP_FAILED) return NULL;

    // Allocate slab descriptor and ctrl from SLB0
    slab s = slb0_alloc(sizeof(sc_slab_s));
    if (!s) goto fail_mem;
    s->base = mem;
    s->size = arena_size;
    s->slab_id = s_next_slab_id++;

    sc_ctrl_base_s *ctrl = NULL;
    if (policy == POLICY_BUMP) {
        struct sc_bump_ctrl_s *b = slb0_alloc(sizeof(struct sc_bump_ctrl_s));
        if (!b) goto fail_slab;
        b->base.policy = POLICY_BUMP;
        b->base.backing = s;
        b->base.struct_size = sizeof(struct sc_bump_ctrl_s);
        b->base.external = true;
        b->base.shutdown = bump_ctrl_shutdown;
        b->cursor = 0;
        b->capacity = arena_size;
        b->frame_depth = 0;
        b->alloc = bump_ctrl_alloc;
        b->reset = bump_ctrl_reset;
        b->frame_begin = bump_ctrl_frame_begin;
        b->frame_end = bump_ctrl_frame_end;
        ctrl = (sc_ctrl_base_s *)b;
    } else {
        struct sc_reclaim_ctrl_s *r = slb0_alloc(sizeof(struct sc_reclaim_ctrl_s));
        if (!r) goto fail_slab;
        r->base.policy = POLICY_RECLAIM;
        r->base.backing = s;
        r->base.struct_size = sizeof(struct sc_reclaim_ctrl_s);
        r->base.external = true;
        r->base.shutdown = reclaim_ctrl_shutdown;
        r->bump = 0;
        r->capacity = arena_size;
        r->free_head = NULL;
        r->seq = 0;
        r->frame_seq = 0;
        r->alloc = reclaim_ctrl_alloc;
        r->free = reclaim_ctrl_free;
        r->realloc = reclaim_ctrl_realloc;
        r->frame_begin = reclaim_ctrl_frame_begin;
        r->frame_end = reclaim_ctrl_frame_end;
        ctrl = (sc_ctrl_base_s *)r;
    }

    sc_trusted_cap_t *cap = slb0_alloc(sizeof(sc_trusted_cap_t));
    if (!cap) goto fail_ctrl;

    cap->reg_slot = slot;
    cap->arena = s;
    cap->ctrl = ctrl;
    cap->alloc_use.ctrl = ctrl;

    uint8_t idx = (uint8_t)(slot - 1);
    if (policy == POLICY_RECLAIM) {
        cap->alloc_use.alloc = s_trusted_alloc_shims[idx];
        cap->alloc_use.release = s_trusted_free_shims[idx];
        cap->alloc_use.resize = s_trusted_realloc_shims[idx];
    } else {
        // Bump ctrl: caller uses cap->ctrl directly (cast to bump_allocator)
        cap->alloc_use.alloc = NULL;
        cap->alloc_use.release = NULL;
        cap->alloc_use.resize = NULL;
    }

    // Stamp register slot and store cap
    sys0_regs()->R[slot] = (addr)ctrl;
    s_trusted_caps[idx] = cap;
    s_next_trusted_slot++;
    return cap;

fail_ctrl:
    slb0_free(ctrl);
fail_slab:
    slb0_free(s);
fail_mem:
    munmap(mem, arena_size);
    return NULL;
}

// ── Trusted-app subsystem grant (FT-14) ───────────────────────────────────
// Parallel pool to trusted_grant — used by SIGMA_ROLE_TRUSTED_APP modules.
// App slots are numbered 1–TRUSTED_APP_SLOT_MAX, independent of R1–R6.
// App-tier arenas are NOT stamped into sys0_regs()->R[] (those are Ring1 only).

static object trusted_app_slot_alloc(uint8_t slot, usize size) {
    sc_trusted_cap_t *cap = s_trusted_app_caps[slot - 1];
    if (!cap || !cap->ctrl) return NULL;
    struct sc_reclaim_ctrl_s *r = (struct sc_reclaim_ctrl_s *)cap->ctrl;
    return r->alloc(r, size);
}

static void trusted_app_slot_free(uint8_t slot, object ptr) {
    sc_trusted_cap_t *cap = s_trusted_app_caps[slot - 1];
    if (!cap || !cap->ctrl || !ptr) return;
    struct sc_reclaim_ctrl_s *r = (struct sc_reclaim_ctrl_s *)cap->ctrl;
    r->free(r, ptr);
}

static object trusted_app_slot_realloc(uint8_t slot, object ptr, usize new_size) {
    sc_trusted_cap_t *cap = s_trusted_app_caps[slot - 1];
    if (!cap || !cap->ctrl) return NULL;
    struct sc_reclaim_ctrl_s *r = (struct sc_reclaim_ctrl_s *)cap->ctrl;
    return r->realloc(r, ptr, new_size);
}

static frame trusted_app_slot_frame_begin(uint8_t slot) {
    sc_trusted_cap_t *cap = s_trusted_app_caps[slot - 1];
    if (!cap || !cap->ctrl) return FRAME_NULL;
    if (cap->ctrl->policy == POLICY_BUMP) {
        struct sc_bump_ctrl_s *b = (struct sc_bump_ctrl_s *)cap->ctrl;
        return b->frame_begin(b);
    }
    struct sc_reclaim_ctrl_s *r = (struct sc_reclaim_ctrl_s *)cap->ctrl;
    return r->frame_begin(r);
}

static void trusted_app_slot_frame_end(uint8_t slot, frame f) {
    sc_trusted_cap_t *cap = s_trusted_app_caps[slot - 1];
    if (!cap || !cap->ctrl) return;
    if (cap->ctrl->policy == POLICY_BUMP) {
        struct sc_bump_ctrl_s *b = (struct sc_bump_ctrl_s *)cap->ctrl;
        b->frame_end(b, f);
        return;
    }
    struct sc_reclaim_ctrl_s *r = (struct sc_reclaim_ctrl_s *)cap->ctrl;
    r->frame_end(r, f);
}

#define TRUSTED_APP_SHIM(N)                                   \
    static object trusted_a##N##_alloc(usize s) {             \
        return trusted_app_slot_alloc(N, s);                  \
    }                                                         \
    static void trusted_a##N##_free(object p) {               \
        trusted_app_slot_free(N, p);                          \
    }                                                         \
    static object trusted_a##N##_realloc(object p, usize s) { \
        return trusted_app_slot_realloc(N, p, s);             \
    }                                                         \
    static frame trusted_a##N##_frame_begin(void) {           \
        return trusted_app_slot_frame_begin(N);               \
    }                                                         \
    static void trusted_a##N##_frame_end(frame f) {           \
        trusted_app_slot_frame_end(N, f);                     \
    }

TRUSTED_APP_SHIM(1)
TRUSTED_APP_SHIM(2)
TRUSTED_APP_SHIM(3)
TRUSTED_APP_SHIM(4)
TRUSTED_APP_SHIM(5)
TRUSTED_APP_SHIM(6)
TRUSTED_APP_SHIM(7)
TRUSTED_APP_SHIM(8)

static const alloc_fn s_trusted_app_alloc_shims[TRUSTED_APP_SLOT_MAX] = {
    trusted_a1_alloc, trusted_a2_alloc, trusted_a3_alloc, trusted_a4_alloc,
    trusted_a5_alloc, trusted_a6_alloc, trusted_a7_alloc, trusted_a8_alloc};
static const free_fn s_trusted_app_free_shims[TRUSTED_APP_SLOT_MAX] = {
    trusted_a1_free, trusted_a2_free, trusted_a3_free, trusted_a4_free,
    trusted_a5_free, trusted_a6_free, trusted_a7_free, trusted_a8_free};
static const realloc_fn s_trusted_app_realloc_shims[TRUSTED_APP_SLOT_MAX] = {
    trusted_a1_realloc, trusted_a2_realloc, trusted_a3_realloc, trusted_a4_realloc,
    trusted_a5_realloc, trusted_a6_realloc, trusted_a7_realloc, trusted_a8_realloc};
static const frame_begin_fn s_trusted_app_frame_begin_shims[TRUSTED_APP_SLOT_MAX] = {
    trusted_a1_frame_begin, trusted_a2_frame_begin, trusted_a3_frame_begin, trusted_a4_frame_begin,
    trusted_a5_frame_begin, trusted_a6_frame_begin, trusted_a7_frame_begin, trusted_a8_frame_begin};
static const frame_end_fn s_trusted_app_frame_end_shims[TRUSTED_APP_SLOT_MAX] = {
    trusted_a1_frame_end, trusted_a2_frame_end, trusted_a3_frame_end, trusted_a4_frame_end,
    trusted_a5_frame_end, trusted_a6_frame_end, trusted_a7_frame_end, trusted_a8_frame_end};

sc_trusted_cap_t *trusted_app_grant(const char *name, usize size, sc_alloc_policy policy) {
    (void)name;
    if (s_next_trusted_app_slot > TRUSTED_APP_SLOT_MAX) return NULL;
    if (policy == POLICY_KERNEL) return NULL;
    if (policy == 0) policy = POLICY_RECLAIM;

    usize arena_size = (size == 0) ? TRUSTED_SLAB_DEFAULT : size;
    uint8_t slot = s_next_trusted_app_slot;

    void *mem = mmap(NULL, arena_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (mem == MAP_FAILED) return NULL;

    slab s = slb0_alloc(sizeof(sc_slab_s));
    if (!s) goto app_fail_mem;
    s->base = mem;
    s->size = arena_size;
    s->slab_id = s_next_slab_id++;

    sc_ctrl_base_s *ctrl = NULL;
    if (policy == POLICY_BUMP) {
        struct sc_bump_ctrl_s *b = slb0_alloc(sizeof(struct sc_bump_ctrl_s));
        if (!b) goto app_fail_slab;
        b->base.policy = POLICY_BUMP;
        b->base.backing = s;
        b->base.struct_size = sizeof(struct sc_bump_ctrl_s);
        b->base.external = true;
        b->base.shutdown = bump_ctrl_shutdown;
        b->cursor = 0;
        b->capacity = arena_size;
        b->frame_depth = 0;
        b->alloc = bump_ctrl_alloc;
        b->reset = bump_ctrl_reset;
        b->frame_begin = bump_ctrl_frame_begin;
        b->frame_end = bump_ctrl_frame_end;
        ctrl = (sc_ctrl_base_s *)b;
    } else {
        struct sc_reclaim_ctrl_s *r = slb0_alloc(sizeof(struct sc_reclaim_ctrl_s));
        if (!r) goto app_fail_slab;
        r->base.policy = POLICY_RECLAIM;
        r->base.backing = s;
        r->base.struct_size = sizeof(struct sc_reclaim_ctrl_s);
        r->base.external = true;
        r->base.shutdown = reclaim_ctrl_shutdown;
        r->bump = 0;
        r->capacity = arena_size;
        r->free_head = NULL;
        r->seq = 0;
        r->frame_seq = 0;
        r->alloc = reclaim_ctrl_alloc;
        r->free = reclaim_ctrl_free;
        r->realloc = reclaim_ctrl_realloc;
        r->frame_begin = reclaim_ctrl_frame_begin;
        r->frame_end = reclaim_ctrl_frame_end;
        ctrl = (sc_ctrl_base_s *)r;
    }

    sc_trusted_cap_t *cap = slb0_alloc(sizeof(sc_trusted_cap_t));
    if (!cap) goto app_fail_ctrl;

    cap->reg_slot = slot;
    cap->arena = s;
    cap->ctrl = ctrl;
    cap->alloc_use.ctrl = ctrl;

    uint8_t idx = (uint8_t)(slot - 1);
    if (policy == POLICY_RECLAIM) {
        cap->alloc_use.alloc = s_trusted_app_alloc_shims[idx];
        cap->alloc_use.release = s_trusted_app_free_shims[idx];
        cap->alloc_use.resize = s_trusted_app_realloc_shims[idx];
    } else {
        cap->alloc_use.alloc = NULL;
        cap->alloc_use.release = NULL;
        cap->alloc_use.resize = NULL;
    }
    cap->alloc_use.frame_begin = s_trusted_app_frame_begin_shims[idx];
    cap->alloc_use.frame_end = s_trusted_app_frame_end_shims[idx];

    // App-tier arenas are NOT stamped into sys0_regs()->R[]; those are Ring1 only.
    s_trusted_app_caps[idx] = cap;
    s_next_trusted_app_slot++;
    return cap;

app_fail_ctrl:
    slb0_free(ctrl);
app_fail_slab:
    slb0_free(s);
app_fail_mem:
    munmap(mem, arena_size);
    return NULL;
}

// ── Bump controller helpers ────────────────────────────────────────────────
static object bump_ctrl_alloc(struct sc_bump_ctrl_s *c, usize size) {
    if (!c || size == 0) return NULL;
    usize aligned = (c->cursor + (usize)(kAlign - 1)) & ~(usize)(kAlign - 1);
    if (aligned + size > c->capacity) return NULL;
    object ptr = (uint8_t *)c->base.backing->base + aligned;
    c->cursor = aligned + size;
    return ptr;
}

static void bump_ctrl_reset(struct sc_bump_ctrl_s *c, bool zero) {
    if (!c) return;
    if (zero) memset(c->base.backing->base, 0, c->capacity);
    c->cursor = 0;
    c->frame_depth = 0;
}

static frame bump_ctrl_frame_begin(struct sc_bump_ctrl_s *c) {
    if (!c || c->frame_depth >= SC_FRAME_DEPTH_MAX) return FRAME_NULL;
    c->frame_stack[c->frame_depth++] = c->cursor;
    return (frame)(c->cursor + 1);  // +1 so cursor==0 never equals FRAME_NULL
}

static void bump_ctrl_frame_end(struct sc_bump_ctrl_s *c, frame f) {
    if (!c || c->frame_depth == 0) return;
    c->cursor = (usize)f - 1;  // reverse the +1 sentinel offset
    c->frame_depth--;
}

static void bump_ctrl_shutdown(sc_ctrl_base_s *base) {
    if (!base || !base->backing) return;
    munmap(base->backing->base, base->backing->size);
    base->backing->base = NULL;
}

// ── Reclaim controller helpers ─────────────────────────────────────────────

// Insert hdr into the free list, maintaining address order, coalescing adjacent blocks.
static void reclaim_free_block(struct sc_reclaim_ctrl_s *c, rc_block_hdr_s *hdr) {
    if (!c || !hdr) return;
    usize hdr_sz = sizeof(rc_block_hdr_s);
    hdr->flags = RC_FLAG_FREE;
    hdr->seq = 0;

    // Find insertion point in address-ordered free list
    rc_block_hdr_s **pp = &c->free_head;
    rc_block_hdr_s *next = c->free_head;
    while (next && next < hdr) {
        pp = (rc_block_hdr_s **)((uint8_t *)next + hdr_sz);
        next = *(rc_block_hdr_s **)((uint8_t *)next + hdr_sz);
    }

    // Link hdr → next
    *(rc_block_hdr_s **)((uint8_t *)hdr + hdr_sz) = next;
    *pp = hdr;

    // Coalesce hdr → next if adjacent
    if (next) {
        uint8_t *hdr_end = (uint8_t *)hdr + hdr_sz + hdr->size;
        if (hdr_end == (uint8_t *)next) {
            hdr->size += hdr_sz + next->size;
            *(rc_block_hdr_s **)((uint8_t *)hdr + hdr_sz) =
                *(rc_block_hdr_s **)((uint8_t *)next + hdr_sz);
        }
    }

    // Coalesce prev → hdr if adjacent (prev is the node that now points to hdr)
    // Re-read: pp was left pointing into the prev node's next-ptr slot (or &free_head).
    // Walk once more to find the predecessor.
    rc_block_hdr_s *prev = NULL;
    rc_block_hdr_s *cur = c->free_head;
    while (cur && cur != hdr) {
        prev = cur;
        cur = *(rc_block_hdr_s **)((uint8_t *)cur + hdr_sz);
    }
    if (prev) {
        uint8_t *prev_end = (uint8_t *)prev + hdr_sz + prev->size;
        if (prev_end == (uint8_t *)hdr) {
            prev->size += hdr_sz + hdr->size;
            *(rc_block_hdr_s **)((uint8_t *)prev + hdr_sz) =
                *(rc_block_hdr_s **)((uint8_t *)hdr + hdr_sz);
        }
    }
}

// Core alloc: first-fit from free list, then bump expansion.
static object reclaim_alloc_from(struct sc_reclaim_ctrl_s *c, usize size, uint32_t seq) {
    usize hdr_sz = sizeof(rc_block_hdr_s);
    usize need = (size + (usize)(kAlign - 1)) & ~(usize)(kAlign - 1);

    // First-fit from free list
    rc_block_hdr_s **pp = &c->free_head;
    rc_block_hdr_s *cur = c->free_head;
    while (cur) {
        rc_block_hdr_s **next_pp = (rc_block_hdr_s **)((uint8_t *)cur + hdr_sz);
        if (cur->size >= need) {
            // Split if remainder can hold a header + at least kAlign bytes
            if (cur->size >= need + hdr_sz + kAlign) {
                rc_block_hdr_s *rem = (rc_block_hdr_s *)((uint8_t *)cur + hdr_sz + need);
                rem->size = cur->size - need - hdr_sz;
                rem->flags = RC_FLAG_FREE;
                rem->seq = 0;
                *(rc_block_hdr_s **)((uint8_t *)rem + hdr_sz) = *next_pp;
                *pp = rem;
            } else {
                *pp = *next_pp;
            }
            cur->size = need;
            cur->flags = RC_FLAG_LIVE;
            cur->seq = seq;
            return (uint8_t *)cur + hdr_sz;
        }
        pp = next_pp;
        cur = *next_pp;
    }

    // Bump expansion
    usize aligned = (c->bump + (usize)(kAlign - 1)) & ~(usize)(kAlign - 1);
    usize total = hdr_sz + need;
    if (aligned + total > c->capacity) return NULL;
    rc_block_hdr_s *hdr = (rc_block_hdr_s *)((uint8_t *)c->base.backing->base + aligned);
    hdr->size = need;
    hdr->seq = seq;
    hdr->flags = RC_FLAG_LIVE;
    c->bump = aligned + total;
    return (uint8_t *)hdr + hdr_sz;
}

static object reclaim_ctrl_alloc(struct sc_reclaim_ctrl_s *c, usize size) {
    if (!c || size == 0) return NULL;
    return reclaim_alloc_from(c, size, c->frame_seq);
}

static void reclaim_ctrl_free(struct sc_reclaim_ctrl_s *c, object ptr) {
    if (!c || !ptr) return;
    rc_block_hdr_s *hdr = (rc_block_hdr_s *)((uint8_t *)ptr - sizeof(rc_block_hdr_s));
    reclaim_free_block(c, hdr);
}

static object reclaim_ctrl_realloc(struct sc_reclaim_ctrl_s *c, object ptr, usize new_size) {
    if (!c) return NULL;
    if (!ptr) return reclaim_ctrl_alloc(c, new_size);
    if (new_size == 0) {
        reclaim_ctrl_free(c, ptr);
        return NULL;
    }

    rc_block_hdr_s *hdr = (rc_block_hdr_s *)((uint8_t *)ptr - sizeof(rc_block_hdr_s));
    usize need = (new_size + (usize)(kAlign - 1)) & ~(usize)(kAlign - 1);

    if (need <= hdr->size) {
        hdr->size = need;
        return ptr;
    }  // shrink in-place

    object newp = reclaim_alloc_from(c, new_size, hdr->seq);
    if (!newp) return NULL;
    memcpy(newp, ptr, hdr->size);
    reclaim_free_block(c, hdr);
    return newp;
}

static frame reclaim_ctrl_frame_begin(struct sc_reclaim_ctrl_s *c) {
    if (!c) return FRAME_NULL;
    c->seq++;
    c->frame_seq = c->seq;
    return (frame)c->seq;  // seq is always >= 1 after increment, never FRAME_NULL(0)
}

static void reclaim_ctrl_frame_end(struct sc_reclaim_ctrl_s *c, frame f) {
    if (!c || f == FRAME_NULL) return;
    uint32_t tag = (uint32_t)f;
    // Walk all live blocks via bump region; free any with seq >= tag
    usize hdr_sz = sizeof(rc_block_hdr_s);
    uint8_t *base = (uint8_t *)c->base.backing->base;
    usize off = 0;
    while (off + hdr_sz <= c->bump) {
        // Align to kAlign to find each header
        usize aligned = (off + (usize)(kAlign - 1)) & ~(usize)(kAlign - 1);
        if (aligned + hdr_sz > c->bump) break;
        rc_block_hdr_s *hdr = (rc_block_hdr_s *)(base + aligned);
        usize step = hdr_sz + hdr->size;
        if (step == 0) break;  // guard against corrupt zero-size
        if (hdr->flags == RC_FLAG_LIVE && hdr->seq >= tag) reclaim_free_block(c, hdr);
        off = aligned + step;
    }
    c->frame_seq = tag > 1 ? tag - 1 : 0;
}

static void reclaim_ctrl_shutdown(sc_ctrl_base_s *base) {
    if (!base || !base->backing) return;
    munmap(base->backing->base, base->backing->size);
    base->backing->base = NULL;
}

// ── Allocator interface instance ──────────────────────────────────────────
const sc_allocator_i Allocator = {
    .acquire = allocator_acquire,
    .release = allocator_release,
    .create_bump = allocator_create_bump,
    .create_reclaim = allocator_create_reclaim,
    .create_custom = allocator_create_custom,
    .register_ctrl = allocator_register_ctrl,
    .alloc = slb0_alloc,
    .dispose = slb0_free,
    .realloc = slb0_realloc,
    .is_ready = allocator_is_ready,
};

// ── Kernel controller (MTIS) helpers ─────────────────────────────────────
// NodePool layout:
//   [0 .. sizeof(knl_nodepool_hdr_s))          — header (40 bytes)
//   [page_alloc_off .. )                        — knl_page_node_s array (grows up)
//   [.. btree_alloc_off]                        — knl_node_s array (grows down)
// Index 0 == KNL_PAGE_NULL / KNL_NODE_NULL (never issued).
// page_nodes: 1-based index into the page array.
// btree_nodes: top-relative index N → top - N*NODE_SIZE (stable across mremap).

static knl_page_node_s *knl_page_at(sc_kernel_ctrl_s *c, uint16_t idx) {
    if (idx == KNL_PAGE_NULL) return NULL;
    knl_page_node_s *base =
        (knl_page_node_s *)((uint8_t *)c->nodepool + sizeof(knl_nodepool_hdr_s));
    return &base[idx - 1];
}

static knl_node_s *knl_node_at(sc_kernel_ctrl_s *c, uint16_t idx) {
    if (idx == KNL_NODE_NULL) return NULL;
    // btree nodes occupy the top of the nodepool region, growing downward.
    // Formula: top - idx*NODE_SIZE is stable across mremap+memmove.
    uint8_t *top = (uint8_t *)c->nodepool + c->nodepool->capacity;
    return (knl_node_s *)(top - (usize)idx * sizeof(knl_node_s));
}

static uint16_t knl_page_index(sc_kernel_ctrl_s *c, knl_page_node_s *pn) {
    knl_page_node_s *base =
        (knl_page_node_s *)((uint8_t *)c->nodepool + sizeof(knl_nodepool_hdr_s));
    return (uint16_t)(pn - base) + 1u;
}

static uint16_t knl_btree_node_index(sc_kernel_ctrl_s *c, knl_node_s *n) {
    uint8_t *top = (uint8_t *)c->nodepool + c->nodepool->capacity;
    usize off = (usize)(top - (uint8_t *)n);
    return (uint16_t)(off / sizeof(knl_node_s));
}

// Grow nodepool by 2× via mremap; memmove btree region to new top.
// After growth, existing top-relative indices remain valid without reindexing.
static bool knl_nodepool_grow(sc_kernel_ctrl_s *c) {
    knl_nodepool_hdr_s *np = c->nodepool;
    usize old_cap = np->capacity;
    usize new_cap = old_cap * 2;

    void *new_base = mremap((void *)np, old_cap, new_cap, MREMAP_MAYMOVE);
    if (new_base == MAP_FAILED) return false;

    c->nodepool = (knl_nodepool_hdr_s *)new_base;
    np = c->nodepool;
    np->capacity = new_cap;

    // Move btree region to the new top so top-relative indices still resolve correctly.
    usize bt_used = old_cap - np->btree_alloc_off;
    if (bt_used > 0) {
        void *old_bt = (uint8_t *)new_base + np->btree_alloc_off;
        void *new_bt = (uint8_t *)new_base + (new_cap - bt_used);
        memmove(new_bt, old_bt, bt_used);
    }
    np->btree_alloc_off = new_cap - bt_used;
    return true;
}

// Allocate a new page_node slot from the nodepool (grows up).
static knl_page_node_s *knl_page_alloc(sc_kernel_ctrl_s *c) {
    knl_nodepool_hdr_s *np = c->nodepool;
    usize next = np->page_alloc_off + sizeof(knl_page_node_s);
    if (next >= np->btree_alloc_off) {
        if (!knl_nodepool_grow(c)) return NULL;
        np = c->nodepool;
        next = np->page_alloc_off + sizeof(knl_page_node_s);
    }
    knl_page_node_s *pn = (knl_page_node_s *)((uint8_t *)np + np->page_alloc_off);
    memset(pn, 0, sizeof(knl_page_node_s));
    np->page_alloc_off = next;
    return pn;
}

// Allocate a new btree_node slot (grows down from top); check recycled list first.
static knl_node_s *knl_btree_alloc(sc_kernel_ctrl_s *c) {
    knl_nodepool_hdr_s *np = c->nodepool;
    if (np->btree_free_head != KNL_NODE_NULL) {
        uint16_t idx = np->btree_free_head;
        knl_node_s *n = knl_node_at(c, idx);
        np->btree_free_head = n->left_idx;  // free list uses left_idx as next
        memset(n, 0, sizeof(knl_node_s));
        return n;
    }
    usize next = np->btree_alloc_off - sizeof(knl_node_s);
    if (next <= np->page_alloc_off) {
        if (!knl_nodepool_grow(c)) return NULL;
        np = c->nodepool;
        next = np->btree_alloc_off - sizeof(knl_node_s);
    }
    np->btree_alloc_off = next;
    knl_node_s *n = (knl_node_s *)((uint8_t *)np + next);
    memset(n, 0, sizeof(knl_node_s));
    return n;
}

// Return a btree_node to the recycled free list.
static void knl_btree_free_node(sc_kernel_ctrl_s *c, uint16_t idx) {
    knl_nodepool_hdr_s *np = c->nodepool;
    knl_node_s *n = knl_node_at(c, idx);
    uint16_t next = np->btree_free_head;
    memset(n, 0, sizeof(knl_node_s));
    n->left_idx = next;
    np->btree_free_head = idx;
}

// ── Skip-list helpers (4-level, P=0.5) ──────────────────────────────────

static int knl_skip_random_level(void) {
    int level = 0;
    static unsigned int seed = 0x12345678;
    seed = seed * 1103515245 + 12345;
    unsigned int r = seed;
    while ((r & 1) && level < KNL_SKIP_LEVELS - 1) {
        level++;
        r >>= 1;
    }
    return level;
}

static uint16_t knl_skip_find_for_size(sc_kernel_ctrl_s *c, usize need) {
    knl_nodepool_hdr_s *np = c->nodepool;
    uint16_t cur = np->skip_head;
    while (cur != KNL_PAGE_NULL) {
        knl_page_node_s *pn = knl_page_at(c, cur);
        // Check 1: best-fit free block in B-tree
        if (knl_btree_find_free(c, cur, need) != KNL_NODE_NULL) return cur;
        // Check 2: remaining bump space
        if ((usize)(KNL_PAGE_SIZE - pn->bump_offset) >= need) return cur;
        cur = pn->forward[0];
    }
    return KNL_PAGE_NULL;
}

static uint16_t knl_skip_find_containing(sc_kernel_ctrl_s *c, addr address) {
    knl_nodepool_hdr_s *np = c->nodepool;
    if (np->skip_head == KNL_PAGE_NULL) return KNL_PAGE_NULL;
    uint16_t cur = np->skip_head;
    // Multi-level descent (does NOT reset cur between levels)
    for (int i = KNL_SKIP_LEVELS - 1; i >= 0; i--) {
        while (cur != KNL_PAGE_NULL) {
            knl_page_node_s *cn = knl_page_at(c, cur);
            if (!cn) return KNL_PAGE_NULL;
            if (address >= cn->page_base && address < cn->page_base + KNL_PAGE_SIZE) return cur;
            uint16_t next = cn->forward[i];
            if (next == KNL_PAGE_NULL) break;
            knl_page_node_s *nn = knl_page_at(c, next);
            if (!nn || nn->page_base > address) break;
            cur = next;
        }
    }
    return KNL_PAGE_NULL;
}

static void knl_skip_insert(sc_kernel_ctrl_s *c, knl_page_node_s *pn) {
    knl_nodepool_hdr_s *np = c->nodepool;
    uint16_t pidx = knl_page_index(c, pn);
    int level = knl_skip_random_level();

    if (np->skip_head == KNL_PAGE_NULL) {
        for (int i = 0; i < KNL_SKIP_LEVELS; i++) pn->forward[i] = KNL_PAGE_NULL;
        np->skip_head = pidx;
        np->page_count++;
        return;
    }

    uint16_t update[KNL_SKIP_LEVELS];
    for (int i = 0; i < KNL_SKIP_LEVELS; i++) update[i] = KNL_PAGE_NULL;
    uint16_t cur = np->skip_head;

    for (int i = KNL_SKIP_LEVELS - 1; i >= 0; i--) {
        if (i < KNL_SKIP_LEVELS - 1)
            cur = (update[i + 1] != KNL_PAGE_NULL) ? update[i + 1] : np->skip_head;
        while (cur != KNL_PAGE_NULL) {
            knl_page_node_s *cn = knl_page_at(c, cur);
            if (!cn || cn->page_base >= pn->page_base) break;
            uint16_t next = cn->forward[i];
            if (next == KNL_PAGE_NULL) {
                update[i] = cur;
                break;
            }
            knl_page_node_s *nn = knl_page_at(c, next);
            if (!nn || nn->page_base >= pn->page_base) {
                update[i] = cur;
                break;
            }
            cur = next;
        }
    }

    for (int i = 0; i <= level; i++) {
        if (update[i] == KNL_PAGE_NULL) {
            pn->forward[i] = np->skip_head;
        } else {
            knl_page_node_s *prev = knl_page_at(c, update[i]);
            if (prev) {
                pn->forward[i] = prev->forward[i];
                prev->forward[i] = pidx;
            }
        }
    }
    for (int i = level + 1; i < KNL_SKIP_LEVELS; i++) pn->forward[i] = KNL_PAGE_NULL;
    if (update[0] == KNL_PAGE_NULL) np->skip_head = pidx;
    np->page_count++;
}

#if 0  // We are not removing pages from the skip list in this implementation, but this is how it
       // would work if we did.
static void knl_skip_remove(sc_kernel_ctrl_s *c, uint16_t pidx) {
    knl_nodepool_hdr_s *np = c->nodepool;
    knl_page_node_s *target = knl_page_at(c, pidx);
    if (!target) return;

    if (np->skip_head == pidx) {
        np->skip_head = target->forward[0];
        if (np->page_count > 0) np->page_count--;
        return;
    }

    uint16_t update[KNL_SKIP_LEVELS];
    for (int i = 0; i < KNL_SKIP_LEVELS; i++) update[i] = KNL_PAGE_NULL;
    for (int i = KNL_SKIP_LEVELS - 1; i >= 0; i--) {
        uint16_t cur = np->skip_head;
        while (cur != KNL_PAGE_NULL && cur != pidx) {
            knl_page_node_s *cn = knl_page_at(c, cur);
            if (!cn) break;
            if (cn->forward[i] == pidx) {
                update[i] = cur;
                break;
            }
            cur = cn->forward[i];
        }
    }
    for (int i = 0; i < KNL_SKIP_LEVELS; i++) {
        if (update[i] != KNL_PAGE_NULL) {
            knl_page_node_s *prev = knl_page_at(c, update[i]);
            if (prev && prev->forward[i] == pidx) prev->forward[i] = target->forward[i];
        }
    }
    if (np->page_count > 0) np->page_count--;
}
#endif

// ── B-tree helpers (BST ordered by start address) ─────────────────────────

static uint16_t knl_btree_insert(sc_kernel_ctrl_s *c, uint16_t pidx, addr start, usize len) {
    knl_node_s *n = knl_btree_alloc(c);
    if (!n) return KNL_NODE_NULL;
    // Re-fetch page after potential mremap in knl_btree_alloc
    knl_page_node_s *pg = knl_page_at(c, pidx);
    n->start = start;
    n->length = (uint32_t)len;
    n->left_idx = KNL_NODE_NULL;
    n->right_idx = KNL_NODE_NULL;
    n->info = 0;  // LIVE

    uint16_t nidx = knl_btree_node_index(c, n);

    if (pg->btree_root == KNL_NODE_NULL) {
        pg->btree_root = nidx;
    } else {
        uint16_t cur = pg->btree_root;
        while (1) {
            knl_node_s *cn = knl_node_at(c, cur);
            if (start < cn->start) {
                if (cn->left_idx == KNL_NODE_NULL) {
                    cn->left_idx = nidx;
                    break;
                }
                cur = cn->left_idx;
            } else {
                if (cn->right_idx == KNL_NODE_NULL) {
                    cn->right_idx = nidx;
                    break;
                }
                cur = cn->right_idx;
            }
        }
    }
    pg->block_count++;
    pg->alloc_count++;
    return nidx;
}

// BST search by start address — O(log n).
static uint16_t knl_btree_page_search(sc_kernel_ctrl_s *c, uint16_t pidx, addr start) {
    knl_page_node_s *pg = knl_page_at(c, pidx);
    if (!pg) return KNL_NODE_NULL;
    uint16_t cur = pg->btree_root;
    while (cur != KNL_NODE_NULL) {
        knl_node_s *n = knl_node_at(c, cur);
        if (start == n->start) return cur;
        cur = (start < n->start) ? n->left_idx : n->right_idx;
    }
    return KNL_NODE_NULL;
}

// Recursive best-fit traversal — visits both subtrees to find smallest fitting block.
static void knl_btree_find_best_recursive(sc_kernel_ctrl_s *c, uint16_t cur, usize need,
                                          uint16_t *best, uint32_t *best_sz) {
    if (cur == KNL_NODE_NULL) return;
    knl_node_s *n = knl_node_at(c, cur);
    if ((n->info & KNL_FREE_FLAG) && n->length >= (uint32_t)need) {
        if (*best == KNL_NODE_NULL || n->length < *best_sz) {
            *best = cur;
            *best_sz = n->length;
        }
    }
    knl_btree_find_best_recursive(c, n->left_idx, need, best, best_sz);
    knl_btree_find_best_recursive(c, n->right_idx, need, best, best_sz);
}

// Find best-fit free block in page's B-tree — O(n tree nodes) recursive.
static uint16_t knl_btree_find_free(sc_kernel_ctrl_s *c, uint16_t pidx, usize need) {
    knl_page_node_s *pg = knl_page_at(c, pidx);
    if (!pg || pg->btree_root == KNL_NODE_NULL) return KNL_NODE_NULL;
    uint16_t best = KNL_NODE_NULL;
    uint32_t best_sz = 0;
    knl_btree_find_best_recursive(c, pg->btree_root, need, &best, &best_sz);
    return best;
}

// BST helper: find leftmost (minimum) node in subtree.
static uint16_t knl_btree_find_min(sc_kernel_ctrl_s *c, uint16_t idx) {
    while (idx != KNL_NODE_NULL) {
        knl_node_s *n = knl_node_at(c, idx);
        if (n->left_idx == KNL_NODE_NULL) return idx;
        idx = n->left_idx;
    }
    return KNL_NODE_NULL;
}

// BST helper: find rightmost (maximum) node in subtree.
static uint16_t knl_btree_find_max(sc_kernel_ctrl_s *c, uint16_t idx) {
    while (idx != KNL_NODE_NULL) {
        knl_node_s *n = knl_node_at(c, idx);
        if (n->right_idx == KNL_NODE_NULL) return idx;
        idx = n->right_idx;
    }
    return KNL_NODE_NULL;
}

// BST in-order predecessor of target (largest node with start < target.start).
static uint16_t knl_btree_find_predecessor(sc_kernel_ctrl_s *c, uint16_t root, uint16_t target) {
    knl_node_s *t = knl_node_at(c, target);
    if (!t) return KNL_NODE_NULL;
    if (t->left_idx != KNL_NODE_NULL) return knl_btree_find_max(c, t->left_idx);
    uint16_t pred = KNL_NODE_NULL;
    uint16_t cur = root;
    while (cur != KNL_NODE_NULL && cur != target) {
        knl_node_s *n = knl_node_at(c, cur);
        if (t->start > n->start) {
            pred = cur;
            cur = n->right_idx;
        } else {
            cur = n->left_idx;
        }
    }
    return pred;
}

// BST in-order successor of target (smallest node with start > target.start).
static uint16_t knl_btree_find_successor(sc_kernel_ctrl_s *c, uint16_t root, uint16_t target) {
    knl_node_s *t = knl_node_at(c, target);
    if (!t) return KNL_NODE_NULL;
    if (t->right_idx != KNL_NODE_NULL) return knl_btree_find_min(c, t->right_idx);
    uint16_t succ = KNL_NODE_NULL;
    uint16_t cur = root;
    while (cur != KNL_NODE_NULL && cur != target) {
        knl_node_s *n = knl_node_at(c, cur);
        if (t->start < n->start) {
            succ = cur;
            cur = n->left_idx;
        } else {
            cur = n->right_idx;
        }
    }
    return succ;
}

// Recursive BST delete — returns new subtree root.
static uint16_t knl_btree_delete_recursive(sc_kernel_ctrl_s *c, uint16_t cur, uint16_t del,
                                           bool *deleted) {
    if (cur == KNL_NODE_NULL) {
        *deleted = false;
        return KNL_NODE_NULL;
    }
    knl_node_s *cn = knl_node_at(c, cur);

    if (cur == del) {
        *deleted = true;
        // Leaf
        if (cn->left_idx == KNL_NODE_NULL && cn->right_idx == KNL_NODE_NULL) {
            knl_btree_free_node(c, cur);
            return KNL_NODE_NULL;
        }
        // Single child
        if (cn->left_idx == KNL_NODE_NULL) {
            uint16_t r = cn->right_idx;
            knl_btree_free_node(c, cur);
            return r;
        }
        if (cn->right_idx == KNL_NODE_NULL) {
            uint16_t l = cn->left_idx;
            knl_btree_free_node(c, cur);
            return l;
        }
        // Two children: replace with in-order successor (min of right subtree)
        uint16_t succ = knl_btree_find_min(c, cn->right_idx);
        knl_node_s *sn = knl_node_at(c, succ);
        cn->start = sn->start;
        cn->length = sn->length;
        cn->info = sn->info;
        bool dummy;
        cn->right_idx = knl_btree_delete_recursive(c, cn->right_idx, succ, &dummy);
        return cur;
    }

    knl_node_s *del_n = knl_node_at(c, del);
    if (!del_n) {
        *deleted = false;
        return cur;
    }
    if (del_n->start < cn->start)
        cn->left_idx = knl_btree_delete_recursive(c, cn->left_idx, del, deleted);
    else
        cn->right_idx = knl_btree_delete_recursive(c, cn->right_idx, del, deleted);
    return cur;
}

// Delete a btree_node from the page's BST — page block_count updated on success.
static void knl_btree_delete(sc_kernel_ctrl_s *c, uint16_t pidx, uint16_t nidx) {
    knl_page_node_s *pg = knl_page_at(c, pidx);
    if (!pg) return;
    bool deleted = false;
    pg->btree_root = knl_btree_delete_recursive(c, pg->btree_root, nidx, &deleted);
    if (deleted) pg->block_count--;
}

// Coalesce freed block with its BST predecessor and successor if they are adjacent and free.
static void knl_btree_coalesce(sc_kernel_ctrl_s *c, uint16_t pidx, uint16_t nidx) {
    knl_page_node_s *pg = knl_page_at(c, pidx);
    if (!pg || pg->btree_root == KNL_NODE_NULL) return;
    knl_node_s *freed = knl_node_at(c, nidx);
    if (!freed || !(freed->info & KNL_FREE_FLAG)) return;

    // Coalesce with predecessor (left address neighbor)
    uint16_t pred = knl_btree_find_predecessor(c, pg->btree_root, nidx);
    if (pred != KNL_NODE_NULL) {
        knl_node_s *pn = knl_node_at(c, pred);
        if ((pn->info & KNL_FREE_FLAG) && pn->start + pn->length == freed->start) {
            pn->length += freed->length;
            knl_btree_delete(c, pidx, nidx);
            nidx = pred;
            freed = pn;
        }
    }

    // Coalesce with successor (right address neighbor)
    uint16_t succ = knl_btree_find_successor(c, pg->btree_root, nidx);
    if (succ != KNL_NODE_NULL) {
        knl_node_s *sn = knl_node_at(c, succ);
        if ((sn->info & KNL_FREE_FLAG) && freed->start + freed->length == sn->start) {
            freed->length += sn->length;
            knl_btree_delete(c, pidx, succ);
        }
    }
}

// ── Page lifecycle ─────────────────────────────────────────────────────────

// Allocate a new KNL_PAGE_SIZE region from the 2 MB kernel arena and register it.
static uint16_t knl_ensure_page(sc_kernel_ctrl_s *c, usize need) {
    // First try existing pages via skip list
    uint16_t pidx = knl_skip_find_for_size(c, need);
    if (pidx != KNL_PAGE_NULL) return pidx;

    // Allocate a new page from the 2 MB arena
    usize aligned = (c->bump + KNL_PAGE_SIZE - 1) & ~(usize)(KNL_PAGE_SIZE - 1);
    if (aligned + KNL_PAGE_SIZE > c->capacity) return KNL_PAGE_NULL;

    addr page_base = (addr)c->base.backing->base + aligned;
    c->bump = aligned + KNL_PAGE_SIZE;

    knl_page_node_s *pn = knl_page_alloc(c);
    if (!pn) return KNL_PAGE_NULL;
    pn->page_base = page_base;
    pn->btree_root = KNL_NODE_NULL;
    pn->block_count = 0;
    pn->alloc_count = 0;
    pn->bump_offset = 0;
    knl_skip_insert(c, pn);
    return knl_page_index(c, pn);
}

// Bump-allocate within a page (tracked in B-tree as LIVE).
static object knl_bump_from_page(sc_kernel_ctrl_s *c, uint16_t pidx, usize need) {
    knl_page_node_s *pn = knl_page_at(c, pidx);
    usize aligned = (pn->bump_offset + KNL_MIN_ALLOC - 1) & ~(usize)(KNL_MIN_ALLOC - 1);
    if (aligned + need > KNL_PAGE_SIZE) return NULL;
    addr ptr = pn->page_base + aligned;
    pn->bump_offset = (uint16_t)(aligned + need);
    knl_btree_insert(c, pidx, ptr, need);
    return (object)(uintptr_t)ptr;
}

// ── Kernel alloc / free / realloc ─────────────────────────────────────────

static object kernel_alloc(sc_kernel_ctrl_s *c, usize size) {
    if (!c || size == 0) return NULL;
    usize need = (size + KNL_MIN_ALLOC - 1) & ~(usize)(KNL_MIN_ALLOC - 1);
    // For allocations larger than one page, use multi-page bump (no B-tree tracking per page)
    if (need > KNL_PAGE_SIZE) {
        usize aligned = (c->bump + KNL_PAGE_SIZE - 1) & ~(usize)(KNL_PAGE_SIZE - 1);
        if (aligned + need > c->capacity) return NULL;
        object ptr = (uint8_t *)c->base.backing->base + aligned;
        c->bump = aligned + need;
        return ptr;
    }

    // Try to reuse a free block from the skip list
    uint16_t pidx = knl_skip_find_for_size(c, need);
    if (pidx != KNL_PAGE_NULL) {
        uint16_t nidx = knl_btree_find_free(c, pidx, need);
        if (nidx != KNL_NODE_NULL) {
            knl_node_s *n = knl_node_at(c, nidx);
            addr ptr = n->start;
            if (n->length > (uint32_t)need + KNL_MIN_ALLOC) {
                // Split: shrink this node to the requested size; insert remainder as free.
                addr rem_start = n->start + need;
                usize rem_len = n->length - need;
                n->length = (uint32_t)need;
                n->info &= (uint16_t)~KNL_FREE_FLAG;
                // knl_btree_insert may mremap — do NOT use n or pg pointers after this.
                uint16_t rem_idx = knl_btree_insert(c, pidx, rem_start, rem_len);
                if (rem_idx != KNL_NODE_NULL) {
                    knl_node_at(c, rem_idx)->info |= KNL_FREE_FLAG;
                    // insert added 1 to alloc_count; undo since remainder is free
                    knl_page_node_s *pg = knl_page_at(c, pidx);
                    if (pg->alloc_count > 0) pg->alloc_count--;
                }
            } else {
                n->info &= (uint16_t)~KNL_FREE_FLAG;
                knl_page_at(c, pidx)->alloc_count++;
            }
            return (object)(uintptr_t)ptr;
        }
        // Page has bump space — fall through to bump allocation
        return knl_bump_from_page(c, pidx, need);
    }

    // Allocate a fresh page
    pidx = knl_ensure_page(c, need);
    if (pidx == KNL_PAGE_NULL) return NULL;
    return knl_bump_from_page(c, pidx, need);
}

static void kernel_free(sc_kernel_ctrl_s *c, object ptr) {
    if (!c || !ptr) return;
    addr address = (addr)(uintptr_t)ptr;
    uint16_t pidx = knl_skip_find_containing(c, address);
    if (pidx == KNL_PAGE_NULL) return;  // large alloc or invalid ptr — ignore
    knl_page_node_s *pg = knl_page_at(c, pidx);

    // BST search O(log n) instead of linear scan
    uint16_t nidx = knl_btree_page_search(c, pidx, address);
    if (nidx == KNL_NODE_NULL) return;
    knl_node_s *n = knl_node_at(c, nidx);
    if (n->info & KNL_FREE_FLAG) return;  // double-free guard
    n->info |= KNL_FREE_FLAG;
    if (pg->alloc_count > 0) pg->alloc_count--;
    knl_btree_coalesce(c, pidx, nidx);
}

static object kernel_realloc(sc_kernel_ctrl_s *c, object ptr, usize new_size) {
    if (!c) return NULL;
    if (!ptr) return kernel_alloc(c, new_size);
    if (new_size == 0) {
        kernel_free(c, ptr);
        return NULL;
    }

    addr address = (addr)(uintptr_t)ptr;
    uint16_t pidx = knl_skip_find_containing(c, address);
    if (pidx == KNL_PAGE_NULL) return NULL;

    // BST search O(log n)
    uint16_t nidx = knl_btree_page_search(c, pidx, address);
    if (nidx == KNL_NODE_NULL) return NULL;
    knl_node_s *n = knl_node_at(c, nidx);
    if (n->info & KNL_FREE_FLAG) return NULL;
    usize old_size = n->length;

    object new_ptr = kernel_alloc(c, new_size);
    if (!new_ptr) return NULL;
    memcpy(new_ptr, ptr, old_size < new_size ? old_size : new_size);
    kernel_free(c, ptr);
    return new_ptr;
}
