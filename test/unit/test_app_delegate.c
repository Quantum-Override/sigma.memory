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
 * File: test_app_delegate.c
 * Description: Test application allocator delegation via Module.set_bootstrap()
 *              (FR-2603-sigma-memory-002)
 */

#include <sigma.core/allocator.h>
#include <sigma.core/application.h>
#include <sigma.core/module.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "internal/memory.h"
#include "memory.h"

#define PASS(msg) printf("  PASS: %s\n", msg)
#define FAIL(msg)                \
    printf("  FAIL: %s\n", msg); \
    failed++

// ── Test: Default weak linkage (no bootstrap) ──────────────────────────────

// Test that Application.get_allocator() falls back to slb0_* when no custom
// allocator is configured. This verifies weak linkage is working.
void test_default_weak_linkage(int *failed) {
    printf("\n=== Test: Default weak linkage (no bootstrap) ===\n");
    init_memory_system();

    // No Application.set_allocator() called → uses weak linkage fallback
    sc_alloc_use_t *use = Application.get_allocator();
    if (use != NULL) {
        PASS("get_allocator() returns non-NULL");
    } else {
        FAIL("get_allocator() returned NULL");
        cleanup_memory_system();
        return;
    }

    // Allocate via Application allocator (should delegate to slb0_*)
    object p = use->alloc(128);
    if (p != NULL) {
        PASS("Allocation via Application.get_allocator() succeeded");
    } else {
        FAIL("Allocation failed");
        cleanup_memory_system();
        return;
    }

    // Verify kernel controller exists
    sc_kernel_ctrl_s *knl = memory_kernel_ctrl();
    if (knl != NULL) {
        PASS("Kernel controller initialized");
    } else {
        FAIL("Kernel controller not initialized");
    }

    // Free and cleanup
    use->release(p);
    PASS("Free via Application.get_allocator() succeeded");

    cleanup_memory_system();
}

// ── Test: Bootstrap pattern with custom allocator ──────────────────────────

static reclaim_allocator s_custom_allocator = NULL;
static sc_alloc_use_t s_custom_alloc_use;  // Must be static - Application stores pointer

// Shim functions to adapt reclaim controller methods to sc_alloc_use_t signature
// (controller methods take ctrl* as first param, but sc_alloc_use_t expects no-param functions)
static object custom_alloc_shim(usize size) {
    return s_custom_allocator->alloc(s_custom_allocator, size);
}

static void custom_free_shim(object ptr) {
    s_custom_allocator->free(s_custom_allocator, ptr);
}

static object custom_realloc_shim(object ptr, usize new_size) {
    return s_custom_allocator->realloc(s_custom_allocator, ptr, new_size);
}

static void bootstrap_custom_allocator(void) {
    // sigma.memory is initialized (SYSTEM module complete)
    // Create custom reclaim allocator before other modules initialize
    s_custom_allocator = Allocator.create_reclaim(256 * 1024);  // 256 KB arena
    if (s_custom_allocator == NULL) {
        printf("  ERROR: Custom allocator creation failed in bootstrap\n");
        return;
    }

    // Populate sc_alloc_use_t using shim functions
    s_custom_alloc_use.ctrl = (sc_ctrl_base_s *)s_custom_allocator;
    s_custom_alloc_use.alloc = custom_alloc_shim;
    s_custom_alloc_use.release = custom_free_shim;
    s_custom_alloc_use.resize = custom_realloc_shim;

    // Configure application-wide allocator (stores pointer, so use must be static)
    Application.set_allocator(&s_custom_alloc_use);
}

// Simulate "other module" that allocates during init (like anvil, sigma.string, etc.)
static object s_other_module_allocation = NULL;

static int simulate_other_module_init(void *ctx) {
    (void)ctx;
    // This simulates an ecosystem module allocating during its init phase.
    // If bootstrap worked correctly, this should use the custom allocator.
    sc_alloc_use_t *alloc = Application.get_allocator();
    s_other_module_allocation = alloc->alloc(128);
    return 0;  // success
}

static const char *sim_deps[] = {"sigma.memory", NULL};
static const sigma_module_t simulate_other_module = {
    .name = "test.simulate.other",
    .version = "0.0.1",
    .role = SIGMA_ROLE_USER,
    .alloc = SIGMA_ALLOC_DEFAULT,
    .deps = sim_deps,
    .init = simulate_other_module_init,
    .shutdown = NULL,
};

__attribute__((constructor)) static void register_simulate_other_module(void) {
    Module.register_module(&simulate_other_module);
}

void test_bootstrap_custom_allocator(int *failed) {
    printf("\n=== Test: Bootstrap pattern with custom allocator ===\n");
    init_memory_system();

    // Register bootstrap hook (runs after SYSTEM, before USER modules)
    Module.set_bootstrap(bootstrap_custom_allocator);

    // Initialize all modules in 3 phases:
    // 1. SYSTEM (sigma.memory)
    // 2. Bootstrap (bootstrap_custom_allocator)
    // 3. USER (simulate_other_module)
    int result = Module.init_all();
    if (result == 0) {
        PASS("Module.init_all() succeeded");
    } else {
        FAIL("Module.init_all() failed");
        cleanup_memory_system();
        return;
    }

    // Verify custom allocator was created during bootstrap
    if (s_custom_allocator != NULL) {
        PASS("Custom allocator initialized in bootstrap");
    } else {
        FAIL("Custom allocator not initialized");
        Module.shutdown_all();
        cleanup_memory_system();
        return;
    }

    // Verify other module allocated successfully
    if (s_other_module_allocation != NULL) {
        PASS("Other module allocation succeeded");
    } else {
        FAIL("Other module allocation failed");
        Module.shutdown_all();
        Allocator.release((sc_ctrl_base_s *)s_custom_allocator);
        s_custom_allocator = NULL;
        cleanup_memory_system();
        return;
    }

    // Verify allocation came from custom slab (not kernel slab)
    sc_slab_s *custom_slab = s_custom_allocator->base.backing;
    if (custom_slab != NULL) {
        PASS("Custom allocator has backing slab");
    } else {
        FAIL("Custom allocator has no backing slab");
        goto cleanup;
    }

    // Address range check: allocation should be within custom arena
    uintptr_t alloc_addr = (uintptr_t)s_other_module_allocation;
    uintptr_t slab_start = (uintptr_t)custom_slab->base;
    uintptr_t slab_end = slab_start + custom_slab->size;

    if (alloc_addr >= slab_start && alloc_addr < slab_end) {
        PASS("Allocation within custom arena bounds");
    } else {
        FAIL("Allocation NOT in custom arena (bootstrap didn't work)");
    }

cleanup:
    // Cleanup: free the allocation via Application allocator
    sc_alloc_use_t *use = Application.get_allocator();
    use->release(s_other_module_allocation);
    s_other_module_allocation = NULL;

    // Release custom allocator BEFORE shutdown (shutdown will cleanup_memory_system)
    Allocator.release((sc_ctrl_base_s *)s_custom_allocator);
    s_custom_allocator = NULL;

    // Shutdown modules (calls cleanup_memory_system internally)
    Module.shutdown_all();
}

// ── Test runner ────────────────────────────────────────────────────────────

int main(void) {
    int failed = 0;

    printf("=== Application Allocator Delegation Tests ===\n");
    printf("FR-2603-sigma-memory-002: slb0_* weak linkage + Module.set_bootstrap()\n");

    test_default_weak_linkage(&failed);
    test_bootstrap_custom_allocator(&failed);

    printf("\n=== Summary ===\n");
    if (failed == 0) {
        printf("All tests passed!\n");
    } else {
        printf("%d test(s) failed\n", failed);
    }

    return failed > 0 ? 1 : 0;
}
