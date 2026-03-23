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
 * File: module.c
 * Description: sigma.memory module descriptor for the sigma.core module system.
 *
 *   Registers sigma.memory as a SIGMA_ROLE_SYSTEM module.  No deps — it is
 *   the base system.  The constructor only stores the descriptor pointer;
 *   all init work runs inside sigma_module_init_all().
 */

#include <sigma.core/module.h>
#include "internal/memory.h"

static int memory_module_init(void *ctx);
static void memory_module_shutdown(void);

static const char *no_deps[] = {NULL};

static const sigma_module_t sigma_memory_module = {
    .name = "sigma.memory",
    .version = "0.3.0",
    .role = SIGMA_ROLE_SYSTEM,
    .alloc = SIGMA_ALLOC_SYSTEM,
    .deps = no_deps,
    .init = memory_module_init,
    .shutdown = memory_module_shutdown,
};

__attribute__((constructor)) static void register_sigma_memory(void) {
    sigma_module_register(&sigma_memory_module);
}

static int memory_module_init(void *ctx) {
    (void)ctx;
    init_memory_system();
    sigma_module_set_trusted_grant(trusted_grant);
    sigma_module_set_trusted_app_grant(trusted_app_grant);
    return OK;
}

static void memory_module_shutdown(void) {
    cleanup_memory_system();
}
