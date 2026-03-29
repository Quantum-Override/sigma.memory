# FR-2603-sigma-core-007: Application Bootstrap Hook (Reference)

**ID**: FR-2603-sigma-core-007 (sigma.core)  
**Project**: sigma.core  
**Requested by**: sigma.memory team  
**Date**: 2026-03-28  
**Status**: RESOLVED (sigma.core v1.2.0)  
**Resolution**: Implemented via `Module.set_bootstrap()` API - see sigma.core documentation "Using The Module System.md"

**Reference**: This is a cross-reference document. The actual FR lives in sigma.core repository at `feature-reqs/FR-2603-sigma-core-007.md`

---

## Problem Statement

Applications using the Sigma module system need to configure global resources (specifically allocators via `Application.set_allocator()`) **before** ecosystem modules initialize and begin allocating memory.

Current module initialization uses topological sort based on declared dependencies. When an application declares dependencies on ecosystem modules (e.g., `deps = ["sigma.memory", "anvil", "sigma.string"]`), those dependencies initialize **before** the application's `init()` hook runs. This creates a timing gap where ecosystem modules allocate using default allocators before the application can configure its preferred allocation strategy.

### Concrete Example

**sigma.test application**:
- Binary with `main()` entry point
- Dependencies: `sigma.memory.o`, `anvil.o`
- Goal: Configure custom reclaim allocator for uniform allocation behavior

**Current execution order**:
```
1. sigma.memory.init()     ✓ (SIGMA_ROLE_SYSTEM)
2. anvil.init()            ← allocates via Application.get_allocator() → default/weak linkage
3. sigma.test.init()       ← TOO LATE: Application.set_allocator() called here
```

**Required execution order**:
```
1. sigma.memory.init()              ✓ (SIGMA_ROLE_SYSTEM)
2. [APPLICATION BOOTSTRAP POINT]    ← Application.set_allocator() happens here
3. anvil.init()                     ← allocates via configured allocator
4. sigma.test.init()                ← completes application-specific setup
```

---

## Requirements

### FR-1: Early Application Setup Hook

The module system **MUST** provide a mechanism for application modules to execute setup logic **after system modules initialize** but **before any other modules initialize**.

**Constraints**:
- Setup logic runs after `SIGMA_ROLE_SYSTEM` modules complete initialization
- Setup logic runs before modules listed in application's `deps` array initialize
- Setup logic can call sigma.core APIs (e.g., `Application.set_allocator()`)
- Failure during setup should abort module initialization sequence

### FR-2: Application Role Recognition

The module system **MUST** distinguish application modules from library/service modules to identify which modules require early setup.

**Constraints**:
- Only modules with appropriate role/designation can use early setup hook
- Multiple applications in same process should be detectable (error condition or defined behavior)

### FR-3: Dependency Order Preservation

The early setup mechanism **MUST NOT** break existing topological dependency ordering for non-application modules.

**Constraints**:
- Ecosystem modules (anvil, sigma.string, etc.) continue to initialize based on declared dependencies
- Application's regular `init()` hook runs in normal topological order (after deps)
- Early setup does not create dependency cycles

---

## Use Cases

### UC-1: Custom Allocator Configuration
Application configures reclaim allocator during early setup. All ecosystem modules use configured allocator during their initialization.

### UC-2: Logging/Diagnostic Bootstrap
Application initializes logging subsystem during early setup. Ecosystem modules can log during their initialization.

### UC-3: Security Context Establishment
Application establishes trust/capability context during early setup. Ecosystem modules operate within established security boundaries.

---

## Out of Scope

- Implementation strategy (function pointer, role-based dispatch, two-phase init, etc.)
- API surface design (hook placement in module descriptor vs. separate registry)
- Performance optimization
- Backwards compatibility migration strategy

---

## Success Criteria

1. **sigma.test** can call `Application.set_allocator()` before **anvil** initializes
2. **anvil** allocations during init use allocator configured by **sigma.test**
3. Existing module initialization order semantics preserved for non-application modules
4. Clear failure mode if multiple applications attempt early setup

---

## Related Work

- `Application.set_allocator()` / `Application.get_allocator()` API (sigma.core existing)
- Weak linkage fallback pattern (`slb0_alloc/free/realloc` symbols)
- Module role system (`SIGMA_ROLE_SYSTEM`, `SIGMA_ROLE_TRUSTED`, `SIGMA_ROLE_TRUSTED_APP`)

---

## Notes

This requirement emerged during implementation of FR-2603-sigma-memory-002 (Application Allocator Delegation) when architectural timing constraints prevented applications from configuring allocators before dependent modules initialized.
