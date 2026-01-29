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
    bool (*set)(void *scope_ptr);
    sbyte (*config)(void *scope_ptr, int mask_type);
    object (*alloc)(void *scope_ptr, usize size);
    void (*dispose)(void *scope_ptr, object ptr);
} sc_allocator_scope_i;

// Top-level allocator facade (uses current scope)
typedef struct sc_allocator_i {
    object (*alloc)(usize size);
    void (*dispose)(object ptr);
    sc_allocator_scope_i Scope;
} sc_allocator_i;
extern const sc_allocator_i Allocator;
#endif
