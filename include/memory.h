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
 * File: memory.h
 * Description: SigmaCore memory management implementation
 *
 * THREAD SAFETY
 * -------------
 * Sigma.Memory is designed for single-threaded, non-concurrent use.
 * It is intentionally thread-FRIENDLY: one independent instance per thread
 * is safe, but sharing a single instance across threads without external
 * synchronisation is undefined behaviour.
 * Threading support (hooks for Sigma.Tasking) is planned for a future release.
 */

#pragma once

#include <sigma.core/types.h>

#if 1  // Region: Scope & SLB Definitions
// Scope configurations
#define SCOPE_POLICY 0x01
#define SCOPE_FLAG 0x02

// Scope policies
enum {
    SCOPE_POLICY_RECLAIMING = 0,  // Special: SYS0 only (reclaiming allocator)
    SCOPE_POLICY_DYNAMIC = 1,     // Auto-grows by chaining pages
    SCOPE_POLICY_FIXED = 2,       // Prealloc only, returns NULL when full
};
// Scope flags
enum {
    SCOPE_FLAG_PROTECTED = 1u << 0,  // Blocks destroy/reset
    SCOPE_FLAG_PINNED = 1u << 1,     // Blocks set_current and frame ops
    SCOPE_FLAG_SECURE = 1u << 2,     // Blocks cross-scope move/copy
};

// Forward declarations
typedef struct sc_scope *scope;
typedef struct sc_frame_marker *frame;
#endif

#if 1  // Region: Allocator Interface
// Per-scope operations (explicit scope required)
typedef struct sc_allocator_scope_i {
    void *(*current)(void);
    integer (*set)(void *scope_ptr);   // pushes R7 stack; returns ERR on overflow
    void (*restore)(void);             // pops R7 stack (reverses most recent set/create)
    sbyte (*config)(void *scope_ptr, int mask_type);
    object (*alloc)(void *scope_ptr, usize size);
    void (*dispose)(void *scope_ptr, object ptr);
} sc_allocator_scope_i;

// Frame sub-interface: explicit scope targeting (v0.2.3)
typedef struct sc_frame_i {
    frame (*begin)(void);                       // begin frame on current scope (R7)
    integer (*end)(frame f);                    // end frame on current scope (R7)
    frame (*begin_in)(scope s);                 // begin frame on named scope, no R7 change
    integer (*end_in)(scope s, frame f);        // end frame on named scope, no R7 change
    usize (*depth)(void);                       // frame depth of current scope (R7)
    usize (*depth_of)(scope s);                 // frame depth of named scope
    usize (*allocated)(frame f);                // bytes allocated within frame
} sc_frame_i;

// Arena sub-interface: lifecycle operations (v0.2.3)
typedef struct sc_arena_i {
    scope (*create)(const char *name, sbyte policy);   // create arena, auto-push R7
    void (*dispose)(scope s);                          // dispose arena, auto-unwind frames, pop R7
    scope (*find)(const char *name);                   // find existing arena by name
    object (*alloc)(usize size);                       // alloc from current scope (must be arena)
    void (*dispose_ptr)(scope s, object ptr);          // dispose ptr from arena
    frame (*frame_begin)(scope s);                     // begin frame in arena (no R7 change)
    integer (*frame_end)(scope s, frame f);            // end frame in arena (no R7 change)
} sc_arena_i;

// Top-level allocator facade (uses current scope)
typedef struct sc_allocator_i {
    object (*alloc)(usize size);
    void (*dispose)(object ptr);
    object (*realloc)(object ptr, usize new_size);  // in-place shrink; alloc+copy+dispose grow
    sc_allocator_scope_i Scope;

    // Frame operations - backward compat facade (uses R7); fixed in v0.2.3
    frame (*frame_begin)(void);
    integer (*frame_end)(frame f);
    usize (*frame_depth)(void);
    usize (*frame_allocated)(frame f);

    // Arena operations - backward compat; fixed in v0.2.3
    scope (*create_arena)(const char *name, sbyte policy);
    void (*dispose_arena)(scope s);

    // Sub-interfaces (v0.2.3)
    sc_frame_i Frame;
    sc_arena_i Arena;
} sc_allocator_i;
extern const sc_allocator_i Allocator;
#endif
