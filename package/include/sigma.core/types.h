/*
 * SigmaCore
 * Copyright (c) 2025 David Boarman (BadKraft) and contributors
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
 * File: sigma.core/types.h
 * Description: Header file for SigmaCore type definitions
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef void *object;
typedef uintptr_t addr;
typedef int64_t integer;
typedef char *string;
typedef size_t usize;
typedef uint8_t byte;
typedef int8_t sbyte;
typedef uint16_t ushort;
typedef uint32_t uint;

// empty addr
#define ADDR_EMPTY ((addr)0)
// size of addr
#define ADDR_SIZE sizeof(addr)

// OK/ERR
#define OK 0
#define ERR -1

#ifndef __bool_true_false_are_defined
#define __bool_true_false_are_defined 1

#define bool _Bool
#define true 1
#define false 0

#endif
