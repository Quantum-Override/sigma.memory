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
 * File: src/stack.c
 * Description: Implementation of internal NodeStack
 */

#include "internal/stack.h"
#include "internal/memory.h"

// Forward declarations - API functions
static int stack_push(addr value);
static int stack_pop(addr *out_value);
static int stack_peek(addr *out_value);
static bool stack_is_empty(void);
static bool stack_is_full(void);
static usize stack_depth(void);
static void stack_clear(void);

// Forward declarations - Helper functions
static addr get_stack_base(void);
static void set_stack_pointer(usize depth);
static usize get_stack_pointer(void);

// Stack state (track depth at SYS0 offset 192)
// Stack depth stored as first 8 bytes of NodeStack region
// Actual stack data starts at offset 1358 (1350 + 8)
#define STACK_DEPTH_OFFSET SYS0_NODE_STACK_OFFSET
#define STACK_DATA_OFFSET (SYS0_NODE_STACK_OFFSET + sizeof(addr))
#define STACK_DATA_SIZE (SYS0_NODE_STACK_SIZE - sizeof(addr))  // 120 bytes
#define STACK_SLOT_COUNT 15  // 120 bytes / 8 bytes per slot

// API function definitions

/**
 * @brief Push value onto stack
 */
static int stack_push(addr value) {
    usize depth = get_stack_pointer();
    if (depth >= STACK_SLOT_COUNT) {
        return ERR;  // Stack full
    }

    addr stack_base = get_stack_base();
    addr *slot = (addr *)(stack_base + (depth * sizeof(addr)));
    *slot = value;

    set_stack_pointer(depth + 1);
    return OK;
}

/**
 * @brief Pop value from stack
 */
static int stack_pop(addr *out_value) {
    if (out_value == NULL) {
        return ERR;
    }

    usize depth = get_stack_pointer();
    if (depth == 0) {
        return ERR;  // Stack empty
    }

    depth--;
    addr stack_base = get_stack_base();
    addr *slot = (addr *)(stack_base + (depth * sizeof(addr)));
    *out_value = *slot;

    set_stack_pointer(depth);
    return OK;
}

/**
 * @brief Peek at top value without removing
 */
static int stack_peek(addr *out_value) {
    if (out_value == NULL) {
        return ERR;
    }

    usize depth = get_stack_pointer();
    if (depth == 0) {
        return ERR;  // Stack empty
    }

    addr stack_base = get_stack_base();
    addr *slot = (addr *)(stack_base + ((depth - 1) * sizeof(addr)));
    *out_value = *slot;

    return OK;
}

/**
 * @brief Check if stack is empty
 */
static bool stack_is_empty(void) {
    return get_stack_pointer() == 0;
}

/**
 * @brief Check if stack is full
 */
static bool stack_is_full(void) {
    return get_stack_pointer() >= STACK_SLOT_COUNT;
}

/**
 * @brief Get current stack depth
 */
static usize stack_depth(void) {
    return get_stack_pointer();
}

/**
 * @brief Clear stack (reset to empty)
 */
static void stack_clear(void) {
    set_stack_pointer(0);
}

// API interface definition
const sc_stack_i Stack = {
    .push = stack_push,
    .pop = stack_pop,
    .peek = stack_peek,
    .is_empty = stack_is_empty,
    .is_full = stack_is_full,
    .depth = stack_depth,
    .clear = stack_clear,
};

// Helper function definitions

/**
 * @brief Get base address of stack data region
 */
static addr get_stack_base(void) {
    addr sys0_base = Memory.get_sys0_base();
    return sys0_base + STACK_DATA_OFFSET;
}

/**
 * @brief Set stack pointer (depth)
 */
static void set_stack_pointer(usize depth) {
    addr sys0_base = Memory.get_sys0_base();
    addr *depth_ptr = (addr *)(sys0_base + STACK_DEPTH_OFFSET);
    *depth_ptr = depth;
}

/**
 * @brief Get stack pointer (depth)
 */
static usize get_stack_pointer(void) {
    addr sys0_base = Memory.get_sys0_base();
    addr *depth_ptr = (addr *)(sys0_base + STACK_DEPTH_OFFSET);
    return *depth_ptr;
}
