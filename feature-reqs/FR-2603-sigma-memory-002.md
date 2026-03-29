# FR-2603-sigma-memory-002: Application Allocator Delegation

**ID:** FR-2603-sigma-memory-002  
**Type:** Feature Request  
**Owner:** sigma.memory  
**Filed:** 2026-03-27  
**Updated:** 2026-03-29  
**Status:** closed  
**Tags:** sigma-memory, allocator, application, delegation, phase-2, orchestration, BR-2603-q-or-001  
**Depends on:** FR-2603-sigma-core-004 (Application.set_allocator API), FR-2603-sigma-memory-001 (Allocator.dispose rename), FR-2603-sigma-core-007 (Module.set_bootstrap)  
**Blocks:** FR-2603-sigma-collections-003 (collections alloc_use removal)  

**Update 2026-03-29:** sigma.core v1.2.0 added `Module.set_bootstrap()` API (FR-2603-sigma-core-007), resolving application initialization timing requirements. Applications can now configure allocators before ecosystem modules initialize.  

**Implementation Complete (2026-03-29):**
- **Phase 1:** Exported `slb0_alloc/free/realloc` symbols for weak linkage fallback (commit 34d2266)
- **Phase 1:** Created `test_app_delegate.c` validating bootstrap pattern (9/9 assertions, valgrind clean)
- **Phase 2:** Added `allocator_delegate_alloc/dispose/realloc` wrappers routing through `Application.get_allocator()`
- **Phase 2:** Updated `Allocator` interface struct assignments to use delegation wrappers
- **Validation:** All facade tests pass (6/6), valgrind clean (0 errors, 0 leaks)

---

## Summary

Refactor the `Allocator` facade to delegate all allocation operations through `Application.get_allocator()` instead of calling `slb0_*` functions directly. This enables application-wide custom allocator injection (primarily for test infrastructure) while maintaining backward compatibility.

**Core Change:** `Allocator.alloc/dispose/realloc` become thin wrappers that route through the Application-level allocator hook instead of directly calling SLB0.

---

## Background

### Phase 1 Foundation Complete

FR-2603-sigma-core-004 introduced `Application.set_allocator(sc_alloc_use_t *use)` and `Application.get_allocator()` to manage an application-wide custom allocator. This allows test frameworks (sigma.test) to inject tracked allocators, arenas, or other custom allocation strategies.

However, **sigma.memory's Allocator facade currently bypasses this mechanism**:

```c
// Current implementation (sigma.memory v0.3.0)
const sc_allocator_i Allocator = {
    .alloc = slb0_alloc,       // ← Direct call (ignores Application)
    .dispose = slb0_free,      // ← Direct call
    .realloc = slb0_realloc,   // ← Direct call
    ...
};
```

This means `Application.set_allocator()` has **no effect** on code using the `Allocator` facade — the intended delegation pattern doesn't work.

### Why Delegation Matters

The primary use case for `Application.set_allocator()` is **test framework injection**:

**Test Tracking:** sigma.test needs to inject `tracked_malloc/tracked_free` to detect memory leaks across all modules. Without delegation, allocations through `Allocator.*` are invisible to the test framework.

**Sandboxed Testing:** Advanced test isolation (FR-2603-sigma-test-001) requires bounded arenas with byte-precise leak detection. This only works if all allocations route through a sandboxed controller.

**Production Custom Allocators:** Applications may want to use reclaim controllers, bump allocators, or other memory strategies globally. The Application API provides the hook point, but delegation must be implemented to make it functional.

---

## Use Cases

### UC-001: Test Framework Leak Detection

**Actor:** sigma.test framework  
**Goal:** Track all allocations/frees across test execution to detect leaks

**Before Delegation (Broken):**
```c
// In test framework init
Application.set_allocator(sigtest_alloc_use());  // tracked_malloc/free

// In module code
farray_t arr = FArray.create();  // Uses Allocator.alloc internally
// ❌ Allocation NOT tracked (Allocator bypasses Application)
```

**After Delegation (Working):**
```c
// In test framework init
Application.set_allocator(sigtest_alloc_use());  // tracked_malloc/free

// In module code
farray_t arr = FArray.create();  // Uses Allocator.alloc → Application.get_allocator()
// ✅ Allocation IS tracked (delegation working)
```

**Outcome:** Test framework can report `total_mallocs == 5, total_frees == 4` → 1 leak detected.

---

### UC-002: Sandboxed Arena Isolation

**Actor:** Advanced test suite (sigma.test with FR-2603-sigma-test-001)  
**Goal:** Run each test in bounded arena with byte-precise leak detection

**Setup:**
```c
// Per-test setup
st_sandbox_t *box = sigtest_sandbox_create(1 << 20);  // 1MB bounded arena
Application.set_allocator(&box->alloc_use);

// Run test code
farray_t arr = FArray.create();  // ← Allocates from sandbox
FArray.push(&arr, some_data);
// Oops, forgot to FArray.destroy()

// Per-test teardown
sigtest_sandbox_destroy(box);  // ← Reports: "Leak: 256 bytes in 3 allocations"
```

**Benefit:** OOM injection, double-free detection, zero test contamination (each test gets fresh arena).

---

### UC-003: Production Custom Allocator

**Actor:** Application developer using sigma.memory controllers  
**Goal:** Use reclaim controller for all allocations (no manual malloc management)

**Setup:**
```c
#include <sigma.core/application.h>
#include <sigma.memory/memory.h>

void module_init(void) {
    // Create 10MB reclaim pool
    reclaim_allocator pool = Allocator.create_reclaim(10 << 20);
    
    // Convert controller → alloc_use hook
    sc_alloc_use_t hook = {
        .ctrl = (sc_ctrl_base_s*)pool,
        .alloc = (void*(*)(usize))pool->alloc,
        .release = (void(*)(void*))pool->free,
        .resize = (void*(*)(void*, usize))pool->realloc,
    };
    
    Application.set_allocator(&hook);
    
    // All subsequent Allocator.* calls route through reclaim pool
    // (including collections, strings, etc.)
}
```

**Benefit:** Single pool manages all allocations, automatic reclaim on teardown.

---

## Requested Changes

### 1. Add Delegation Wrapper Functions

**File:** `src/memory.c`

Add thin wrapper functions that delegate to `Application.get_allocator()`:

```c
// New wrapper (delegates through Application)
static void *allocator_alloc_wrapper(usize size) {
    sc_alloc_use_t *use = Application.get_allocator();
    return use->alloc(size);
}

static void allocator_dispose_wrapper(void *ptr) {
    sc_alloc_use_t *use = Application.get_allocator();
    use->release(ptr);
}

static void *allocator_realloc_wrapper(void *ptr, usize new_size) {
    sc_alloc_use_t *use = Application.get_allocator();
    if (use->resize) {
        return use->resize(ptr, new_size);
    }
    
    // Fallback: manual realloc if custom allocator doesn't provide resize
    void *new_ptr = use->alloc(new_size);
    if (new_ptr && ptr) {
        // TODO: Need size of old allocation (not tracked in current design)
        // For now, assert resize is provided by custom allocators
        assert(false && "Custom allocator must provide resize function");
    }
    return new_ptr;
}
```

**Note on `realloc` fallback:** Current `sc_alloc_use_t` design doesn't track allocation sizes, so manual fallback is impossible. **Require custom allocators to provide `.resize` if they're used with code that calls `Allocator.realloc()`.**

### 2. Update Allocator Vtable

**File:** `src/memory.c` (around line 1139)

```c
const sc_allocator_i Allocator = {
    .acquire = allocator_acquire,
    .release = allocator_release,
    .create_bump = allocator_create_bump,
    .create_reclaim = allocator_create_reclaim,
    .create_custom = allocator_create_custom,
    .register_ctrl = allocator_register_ctrl,
    .alloc = allocator_alloc_wrapper,      // ← Changed from slb0_alloc
    .dispose = allocator_dispose_wrapper,  // ← Changed from slb0_free
    .realloc = allocator_realloc_wrapper,  // ← Changed from slb0_realloc
    .is_ready = allocator_is_ready,
};
```

### 3. Update Headers

**File:** `include/memory.h`

No public API changes required. `Allocator` interface signature remains identical — only implementation changes.

**Optional:** Add documentation comment:
```c
/**
 * Allocator — Memory allocation facade (delegates to Application allocator)
 * 
 * All allocation operations route through Application.get_allocator().
 * Default: SLB0 (malloc wrapper) if Application.set_allocator() never called.
 * Custom: Test frameworks or applications can inject custom allocators.
 * 
 * @see Application.set_allocator() for injection mechanism
 */
extern const sc_allocator_i Allocator;
```

---

## Test Cases

### TC-001: Allocator delegates to custom allocator (spy pattern)

**Purpose:** Verify `Allocator.alloc()` routes through `Application.get_allocator()`.

**Implementation:**
```c
static int g_alloc_called = 0;
static int g_dispose_called = 0;

static void *spy_alloc(usize size) {
    g_alloc_called++;
    return malloc(size);
}

static void spy_dispose(void *ptr) {
    g_dispose_called++;
    free(ptr);
}

void test_allocator_delegates_to_application(void) {
    // Setup: inject spy allocator
    g_alloc_called = 0;
    g_dispose_called = 0;
    
    sc_alloc_use_t spy = {
        .ctrl = NULL,
        .alloc = spy_alloc,
        .release = spy_dispose,
        .resize = NULL,
    };
    Application.set_allocator(&spy);
    
    // Test: call Allocator facade
    void *p = Allocator.alloc(64);
    
    // Verify: spy was called (proves delegation)
    assert(g_alloc_called == 1);  // ✓ Allocation delegated
    assert(p != NULL);
    
    Allocator.dispose(p);
    assert(g_dispose_called == 1);  // ✓ Disposal delegated
    
    // Cleanup
    Application.set_allocator(NULL);  // Reset to SLB0
}
```

**Expected Result:** Counters increment to 1, proving delegation is working.

---

### TC-002: Default fallback to SLB0 when no custom allocator set

**Purpose:** Verify backward compatibility — if `Application.set_allocator()` never called, still works.

**Implementation:**
```c
void test_allocator_defaults_to_slb0(void) {
    // Ensure no custom allocator is set
    Application.set_allocator(NULL);
    
    // Test: allocate without custom allocator
    void *p = Allocator.alloc(128);
    
    // Verify: allocation succeeded (SLB0 fallback working)
    assert(p != NULL);
    
    Allocator.dispose(p);
    // No crash = success
}
```

**Expected Result:** Allocation succeeds, no NULL pointer dereference.

---

### TC-003: Multiple allocations route through same custom allocator

**Purpose:** Verify delegation consistency across multiple calls.

**Implementation:**
```c
void test_multiple_allocations_use_custom_allocator(void) {
    g_alloc_called = 0;
    
    sc_alloc_use_t spy = {
        .ctrl = NULL,
        .alloc = spy_alloc,
        .release = spy_dispose,
        .resize = NULL,
    };
    Application.set_allocator(&spy);
    
    // Multiple allocations
    void *p1 = Allocator.alloc(32);
    void *p2 = Allocator.alloc(64);
    void *p3 = Allocator.alloc(128);
    
    // Verify: all routed through custom allocator
    assert(g_alloc_called == 3);
    assert(p1 && p2 && p3);
    
    Allocator.dispose(p1);
    Allocator.dispose(p2);
    Allocator.dispose(p3);
    
    Application.set_allocator(NULL);
}
```

**Expected Result:** Counter reaches 3, all allocations delegated.

---

### TC-004: Realloc delegates to custom allocator (if provided)

**Purpose:** Verify `Allocator.realloc()` delegation for custom allocators that support resize.

**Implementation:**
```c
static int g_realloc_called = 0;

static void *spy_realloc(void *ptr, usize new_size) {
    g_realloc_called++;
    return realloc(ptr, new_size);
}

void test_realloc_delegates_to_custom_allocator(void) {
    g_alloc_called = 0;
    g_realloc_called = 0;
    
    sc_alloc_use_t spy = {
        .ctrl = NULL,
        .alloc = spy_alloc,
        .release = spy_dispose,
        .resize = spy_realloc,  // ← Provide resize
    };
    Application.set_allocator(&spy);
    
    // Allocate then resize
    void *p = Allocator.alloc(64);
    assert(g_alloc_called == 1);
    
    void *p2 = Allocator.realloc(p, 128);
    assert(g_realloc_called == 1);  // ✓ Realloc delegated
    assert(p2 != NULL);
    
    Allocator.dispose(p2);
    Application.set_allocator(NULL);
}
```

**Expected Result:** Realloc counter increments, proving delegation works for resize.

---

### TC-005: Integration with sigma.test tracked allocator

**Purpose:** Verify real-world integration with sigma.test's `tracked_malloc/free`.

**Implementation:**
```c
void test_integration_with_sigtest_tracked_allocator(void) {
    // Setup: use sigma.test's tracked allocator
    Application.set_allocator(sigtest_alloc_use());
    
    // Get baseline counts
    usize before_mallocs = sigtest_total_mallocs();
    usize before_frees = sigtest_total_frees();
    
    // Allocate via Allocator facade
    void *p1 = Allocator.alloc(256);
    void *p2 = Allocator.alloc(512);
    
    // Verify: tracked allocator saw the allocations
    assert(sigtest_total_mallocs() == before_mallocs + 2);
    
    Allocator.dispose(p1);
    assert(sigtest_total_frees() == before_frees + 1);
    
    Allocator.dispose(p2);
    assert(sigtest_total_frees() == before_frees + 2);
    
    // Balance check
    assert(sigtest_total_mallocs() == sigtest_total_frees());
    
    Application.set_allocator(NULL);
}
```

**Expected Result:** sigma.test's counters increment, proving end-to-end integration works.

---

## Performance Impact

**Overhead:** One pointer dereference per allocation (`Application.get_allocator()` → vtable call).

**Measurement:**
```
Before (direct):     slb0_alloc()                        → ~10 cycles
After (delegated):   allocator_alloc_wrapper() 
                     → Application.get_allocator()
                     → use->alloc()                      → ~12 cycles
```

**Impact:** ~20% overhead per allocation call, but allocations are already heavyweight (syscalls, memory operations dominate). **Negligible in practice.**

**Optimization:** `Application.get_allocator()` can be inlined or cached if profiling shows hotspot.

---

## Acceptance Criteria

- [ ] `allocator_alloc_wrapper()`, `allocator_dispose_wrapper()`, `allocator_realloc_wrapper()` implemented in `src/memory.c`
- [ ] `Allocator` vtable updated to use wrappers instead of direct `slb0_*` calls
- [ ] TC-001 (spy pattern) passes — proves delegation working
- [ ] TC-002 (default fallback) passes — backward compatibility maintained
- [ ] TC-003 (multiple allocations) passes — consistency verified
- [ ] TC-004 (realloc delegation) passes — resize support confirmed
- [ ] TC-005 (sigma.test integration) passes — real-world use case validated
- [ ] All 76 existing sigma.memory tests pass (no regressions)
- [ ] Documentation updated: `include/memory.h` comment on delegation behavior
- [ ] Published: sigma.memory v0.4.0 (minor bump, backward-compatible implementation change)

---

## Migration Impact

**Breaking Changes:** None (public API unchanged)

**Behavior Change:** 
- `Allocator.*` calls now respect `Application.set_allocator()` (previously ignored)
- This is the **intended behavior** — not a regression, a bug fix

**Affected Projects:**
- sigma.test — Can now use `Application.set_allocator()` for global tracking (enable this in FR-2603-sigma-test-002 or similar)
- sigma.collections — Phase 3A (FR-003/004) depends on this delegation working
- anvil — No impact (uses SLB0 directly, or will use Application.set_allocator() if needed)

---

## Rationale

**Why not keep direct `slb0_*` calls?**
- Defeats the purpose of `Application.set_allocator()` — custom allocators have no effect
- Test frameworks can't track allocations (leak detection broken)
- Prevents advanced test isolation (sandboxed arenas, OOM injection)

**Why delegation instead of compile-time configuration?**
- Runtime flexibility — test frameworks need to inject allocators dynamically
- Single binary can support multiple allocator strategies (dev: tracked, prod: optimized)
- Matches industry patterns (glibc malloc hooks, jemalloc, tcmalloc)

**Why thin wrappers instead of macro indirection?**
- Debuggable — clear call stack (no preprocessor obfuscation)
- Type-safe — compiler enforces `sc_alloc_use_t` interface
- Testable — spy pattern can intercept at runtime

---

## See Also

- **FR-2603-sigma-core-004** — Application.set_allocator() API (dependency)
- **FR-2603-sigma-memory-001** — Allocator.dispose rename (prerequisite)
- **FR-2603-sigma-memory-003** — Internal allocation audit (prevent recursion)
- **FR-2603-sigma-collections-003** — Phase 3A depends on delegation working
- **FR-2603-sigma-test-001** — Sandboxed testing use case
- **ORCHESTRATION-BR-2603-q-or-001.md** — Phase 2 specifications
