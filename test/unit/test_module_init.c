/*
 * Standalone isolation test: sigma_module_init_all() -> Allocator.alloc()
 * Mirrors exactly what sigma.test's main() does — no direct init_memory_system() call.
 * No sigma.test dependency.
 */
#include <sigma.core/module.h>
#include <stdio.h>
#include <string.h>
#include "internal/memory.h"
#include "memory.h"

#define PASS(msg) printf("  PASS: %s\n", msg)
#define FAIL(msg)                \
    printf("  FAIL: %s\n", msg); \
    failed++

int main(void) {
    int failed = 0;

    printf("=== Module system isolation test ===\n\n");

    /* 1. Before sigma_module_init_all() — memory must NOT be ready */
    if (!Allocator.is_ready()) {
        PASS("is_ready() == false before sigma_module_init_all()");
    } else {
        FAIL("is_ready() unexpectedly true before sigma_module_init_all()");
    }

    /* 2. Init via module system — same path sigma.test uses */
    sigma_module_init_all();

    /* 3. Memory must be ready now */
    if (Allocator.is_ready()) {
        PASS("is_ready() == true after sigma_module_init_all()");
    } else {
        FAIL("is_ready() == false after sigma_module_init_all() — module init did not run");
        printf("  state = 0x%02x\n", memory_state());
        return 1;
    }

    /* 4. Alloc immediately after module init */
    object p = Allocator.alloc(64);
    if (p != NULL) {
        PASS("Allocator.alloc(64) returned non-NULL");
    } else {
        FAIL("Allocator.alloc(64) returned NULL");
    }

    /* 5. Memory is writable */
    if (p) {
        memset(p, 0xCD, 64);
        if (((unsigned char *)p)[0] == 0xCD && ((unsigned char *)p)[63] == 0xCD) {
            PASS("allocated memory is writable and readable");
        } else {
            FAIL("memory write/read mismatch");
        }
        Allocator.free(p);
        PASS("Allocator.free() did not crash");
    }

    /* 6. Shutdown via module system */
    sigma_module_shutdown_all();

    /* 7. is_ready() must be false after shutdown */
    if (!Allocator.is_ready()) {
        PASS("is_ready() == false after sigma_module_shutdown_all()");
    } else {
        FAIL("is_ready() still true after sigma_module_shutdown_all()");
    }

    printf("\n=== %s (%d failure(s)) ===\n", failed == 0 ? "ALL PASSED" : "FAILED", failed);
    return failed > 0 ? 1 : 0;
}
