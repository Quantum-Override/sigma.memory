/*
 * Standalone isolation test: init_memory_system() -> Allocator.alloc()
 * No sigma.test dependency. No constructors. No module system.
 * Compile and run directly.
 */
#include <stdio.h>
#include <string.h>
#include "internal/memory.h"
#include "sigma.memory/memory.h"

#define PASS(msg) printf("  PASS: %s\n", msg)
#define FAIL(msg)                \
    printf("  FAIL: %s\n", msg); \
    failed++

int main(void) {
    int failed = 0;

    printf("=== Direct memory isolation test ===\n\n");

    /* 1. is_ready() must be false before init */
    if (!Allocator.is_ready()) {
        PASS("is_ready() == false before init");
    } else {
        FAIL("is_ready() unexpectedly true before init");
    }

    /* 2. Init */
    init_memory_system();

    /* 3. is_ready() must be true after init */
    if (Allocator.is_ready()) {
        PASS("is_ready() == true after init");
    } else {
        FAIL("is_ready() == false after init — bootstrap failed");
        printf("\n  state = 0x%02x\n", memory_state());
        return 1;  // nothing further will work
    }

    /* 4. Alloc immediately after init */
    object p = Allocator.alloc(64);
    if (p != NULL) {
        PASS("Allocator.alloc(64) returned non-NULL");
    } else {
        FAIL("Allocator.alloc(64) returned NULL");
    }

    /* 5. Memory is writable */
    if (p) {
        memset(p, 0xAB, 64);
        if (((unsigned char *)p)[0] == 0xAB && ((unsigned char *)p)[63] == 0xAB) {
            PASS("allocated memory is writable and readable");
        } else {
            FAIL("memory write/read mismatch");
        }
        Allocator.free(p);
        PASS("Allocator.free() did not crash");
    }

    /* 6. Repeated alloc/free */
    for (int i = 0; i < 8; i++) {
        object q = Allocator.alloc(128);
        if (!q) {
            FAIL("alloc returned NULL in repeat loop");
            break;
        }
        Allocator.free(q);
    }
    if (failed == 0) PASS("8x alloc/free loop stable");

    /* 7. Cleanup */
    cleanup_memory_system();

    /* 8. is_ready() must be false after cleanup */
    if (!Allocator.is_ready()) {
        PASS("is_ready() == false after cleanup");
    } else {
        FAIL("is_ready() still true after cleanup");
    }

    printf("\n=== %s (%d failure(s)) ===\n", failed == 0 ? "ALL PASSED" : "FAILED", failed);
    return failed > 0 ? 1 : 0;
}
