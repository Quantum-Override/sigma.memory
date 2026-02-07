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
 * File: internal/stack.h
 * Description: Internal NodeStack for B-Tree operations
 *
 * Operation Convention:
 * 1. Push parameters onto stack
 * 2. Call operation
 * 3. Result returned in R2 register
 * 4. Operation clears stack
 *
 * Example:
 *   Stack.push(size);
 *   Stack.push(root_idx);
 *   btree_search();  // R2 = found node_idx or NODE_NULL
 */

#pragma once

#include <sigma.core/types.h>

// Stack constants
#define STACK_CAPACITY 16  // 16 slots (128 bytes / 8 bytes per slot)

/**
 * @brief Internal NodeStack interface
 *
 * Stack resides in SYS0 @ offset 1350 (128 bytes)
 * Used for passing parameters to B-Tree operations
 * Results returned via R2 register
 */
typedef struct sc_stack_i {
    /**
     * @brief Push value onto stack
     * @param value 8-byte value to push
     * @return OK on success, ERR if stack full
     */
    int (*push)(addr value);

    /**
     * @brief Pop value from stack
     * @param out_value Pointer to receive popped value
     * @return OK on success, ERR if stack empty
     */
    int (*pop)(addr *out_value);

    /**
     * @brief Peek at top value without removing
     * @param out_value Pointer to receive top value
     * @return OK on success, ERR if stack empty
     */
    int (*peek)(addr *out_value);

    /**
     * @brief Check if stack is empty
     * @return true if empty, false otherwise
     */
    bool (*is_empty)(void);

    /**
     * @brief Check if stack is full
     * @return true if full, false otherwise
     */
    bool (*is_full)(void);

    /**
     * @brief Get current stack depth
     * @return Number of items on stack
     */
    usize (*depth)(void);

    /**
     * @brief Clear stack (reset to empty)
     */
    void (*clear)(void);
} sc_stack_i;

extern const sc_stack_i Stack;
