# BUG-001 — Arena Stale Pointer After Dispose

**Date:** 2026-03-13
**Reported by:** anvil project (Anvil)
**Severity:** Critical — SIGSEGV, silent data corruption
**Status:** Root cause identified; Anvil-side fix required + FT-12 planned (see Resolution)

---

## Summary

[Reported]
`Allocator.Arena.create()` + `Allocator.Scope.restore()` + `Allocator.Arena.dispose()` crashes
when two or more arenas are alive concurrently.

[Analysis]
`Arena.dispose()` immediately frees and zeroes the scope-table slot. Any pointer to that
slot held by the caller is a **dangling pointer** from that moment forward. If a second
`Arena.create()` runs before all holders of the old pointer call `dispose`, the new arena
recycles the same slot address, and a subsequent `dispose` on the stale pointer tears down
live state → crash at `nodepool_shutdown`.

---

## Observed Behavior

1. `Arena.create("name", SCOPE_POLICY_DYNAMIC)` — creates scope-table slot, auto-pushes R7.
2. `Scope.restore()` — pops R7 back to the prior scope (reclaiming allocator).

   [Reported] **Side-effect:** the scope-table slot is marked as free and becomes eligible for reuse by a subsequent `Arena.create`.

   [Analysis] `Scope.restore()` does **not** mark the slot as free. It only clears `current->prev` and restores R7. Slot eligibility is determined solely by `nodepool_base == ADDR_EMPTY`, which is only set by `Arena.dispose()`. The root cause is not here.
3. A second `Arena.create(...)` call reuses the **same slot address** returned in step 1.

   [Analysis] This happens only after `Arena.dispose()` freed the slot (`nodepool_base == ADDR_EMPTY`). The stale pointer in `ctx->arena` still holds the old slot address. Nothing in the Anvil code path nulled or guarded it after `Arena.dispose()`.
4. Now two live `context` objects hold `ctx->arena` pointing to the **same slot address**.

   [Analysis] First context disposed → slot freed → second `Arena.create` recycled the slot → first context's `ctx->arena` is now an alias for the second context's live slot. This is an Anvil ownership bug, not a sigma.memory bug.
5. The first `Arena.dispose(slot)` succeeds (disposes the second arena's live state).
6. The second `Arena.dispose(slot)` hits an already-freed nodepool → SIGSEGV.

### Crash stack (representative)

```
#0  nodepool_shutdown (scope_ptr=<sys_page+N>) at src/node_pool.c:329
#1  arena_dispose_impl (s=<sys_page+N>)        at src/memory.c:1623
#2  context_dispose ()                          at src/core/context.c
#3  set_teardown ()                             at test/benchmarks/bench_serializer.c:74
```

> [Analysis] The crash is at `nodepool_shutdown` because it reads `header->capacity` from an
> already-`munmap`'d region (the nodepool was freed by the first dispose). This is a
> use-after-free on a mapping that no longer exists, hence SIGSEGV.

### Confirming evidence

The debug print added temporarily to `context_dispose` showed every round-trip context
(105+ iterations) using the **exact same slot address** (`sys_page+0x6d0`).

> [Analysis] This confirms slot recycling, but the recycling is correct behavior: the same slot
> address reappears because it was freed by `Arena.dispose`, then immediately reclaimed by
> the next `Arena.create`. The confirming evidence is evidence of a stale pointer in Anvil,
> not a sigma.memory defect.

---

## Repro Pattern

[Reported]
```c
// Two concurrent arenas, same lifecycle as described above:
scope a1 = Allocator.Arena.create("ctx_a", SCOPE_POLICY_DYNAMIC);
Allocator.Scope.restore();     // <-- marks slot_A as free

scope a2 = Allocator.Arena.create("ctx_b", SCOPE_POLICY_DYNAMIC);
Allocator.Scope.restore();     // a2 == a1 (slot reused)

// ... use both arenas via Scope.alloc(a1/a2, ...) ...

Allocator.Arena.dispose(a2);   // frees slot
Allocator.Arena.dispose(a1);   // double-free -> CRASH
```

[Analysis]
```c
// Two concurrent arenas, same lifecycle as described above:
scope a1 = Allocator.Arena.create("ctx_a", SCOPE_POLICY_DYNAMIC);
Allocator.Scope.restore();     // R7 restored — a1 slot is still live, NOT freed

// ... time passes, a1 is disposed somewhere ...
Allocator.Arena.dispose(a1);   // slot freed, nodepool munmap'd
                               // a1 is NOW a dangling pointer

scope a2 = Allocator.Arena.create("ctx_b", SCOPE_POLICY_DYNAMIC);
// a2 == a1 (same slot address recycled — correct behavior)

Allocator.Arena.dispose(a2);   // frees slot correctly
Allocator.Arena.dispose(a1);   // use-after-free: a1 is stale -> CRASH
```

---

## What We Need → sigma.memory Response

[Reported]
- **Option A** — `Scope.restore()` should **not** free the arena's scope-table slot.
  The slot should only be freed when `Arena.dispose()` is called.
- **Option B** — Provide `Arena.create_detached()` (or equivalent) that creates an arena
  **without** auto-pushing R7 and **without** a scope-table slot that participates in R7's
  lifetime tracking. The corresponding `Arena.dispose_detached()` would free only the arena pages.
- **Option C** — Provide `Arena.dispose_no_pop()` — frees the arena pages and the scope-table
  slot without attempting to touch R7. Safe to call regardless of whether the arena is current R7.

[Analysis]
- **Option A** — `Scope.restore()` already does not free the scope-table slot. No change needed here.
- **Option B / Option C** — These are variations of `SCOPE_POLICY_RESOURCE` (FT-12, see Resolution
  below), which is planned. They are not bug fixes.

R7 is a **single register**. It is not a stack data structure managed by sigma.memory.
Pushing and popping scopes via `Arena.create` / `Scope.restore` is caller-managed context
switching. If you need to nest or interleave contexts, that is application-level
orchestration — sigma.memory will not maintain a context stack on your behalf.

---

## Required Fixes (Anvil)

### Fix 1 — Null `ctx->arena` after `Arena.dispose`

The pointer is valid up to the `dispose` call and dangling after. Always:

```c
Allocator.Arena.dispose(ctx->arena);
ctx->arena = NULL;   // <-- mandatory: pointer is invalid from this point forward
```

Never retain, alias, or dereference a scope pointer after its `dispose` call. The slot
may be reallocated to a new arena within the same call frame.

### Fix 2 — Implement `context_stack[]` for nested scopes

R7 is not a stack. If `builder_build` creates arenas that must co-exist across call
boundaries with independent lifetimes, Anvil must manage that explicitly:

```c
// Anvil-side context stack (sketch):
static scope context_stack[MAX_CONTEXTS];
static int   context_top = 0;

void context_push(scope s) {
    context_stack[context_top++] = s;
    Allocator.Scope.set(s);     // activate
}

scope context_pop(void) {
    scope s = context_stack[--context_top];
    Allocator.Scope.restore();  // return R7 to predecessor
    return s;
}
```

Dispose contexts in the order your stack dictates, not in arbitrary order. Non-LIFO
disposal corrupts R7 because `s->prev` encodes the activation order at push time.

---

## Current Workaround (Anvil)

[Reported]
`Allocator.Scope.restore()` is **not called** after `Arena.create()` in `builder_build`.
R7 stays set to the context's arena until `context_dispose()` calls `Arena.dispose()`.
This means callers allocating through R7 (e.g. serializer string builders) place their
allocations inside the context's arena; those are reclaimed with the parse tree when
the context is disposed.

Contexts **must be disposed in LIFO order** (last created → first disposed) so that
`Arena.dispose` always encounters the arena as the current R7 and pops cleanly back to
the preceding scope. Non-LIFO disposal corrupts R7.

This constraint is documented in `builder_build` in `src/core/context.c` and enforced
in the benchmark teardown functions.

[Analysis]
This workaround is correct and safe as long as the LIFO constraint is enforced. It is
not a long-term solution because it forces callers into a strict ordering that does not
match Anvil's natural context lifecycle. FT-12 removes this constraint entirely.

---

## Resolution — FT-12: `SCOPE_POLICY_RESOURCE`

FT-12 is a planned sigma.memory feature that directly solves Anvil's underlying need.

**What it provides:**

```c
// Acquire a private memory slab of the requested size.
// Returns a scope pointer. R7 is NOT touched. No scope-table slot is used.
scope s = Allocator.acquire(size);

// Bump-allocate within the slab.
object ptr = Scope.alloc(s, item_size);

// Reset the slab (reclaim all allocations, keep the mapping).
Scope.reset(s);

// Release the slab entirely.
Allocator.release(s);
```

**How it solves Anvil's problem:**

- No R7 coupling — acquiring a resource scope does not change the active allocator.
  The context arena and the active allocator are entirely independent.
- No scope-table slot — resource scopes are not in the 2–15 range. No recycling
  hazard, no LIFO constraint.
- Dispose in any order — `Allocator.release` frees only the slab for that scope.
  Other scopes are unaffected.
- `ctx->arena` becomes a resource scope. Lifetime is explicit: acquire at context
  creation, release at context disposal. No interaction with R7 at all.

FT-12 is tracked in `docs/backlog.md` in the sigma.memory repository.
