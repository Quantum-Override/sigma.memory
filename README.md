# Sigma.Memory

**Version:** 0.3.0 (in development)  
**Rewrite of:** [`../archive/sigma.mem_0.2/`](../archive/sigma.mem_0.2/)  
**Design:** [`docs/design.md`](docs/design.md)

---

## What Changed

v0.3.0 is a ground-up rewrite driven by three problems found in v0.2.x:

1. **String disposal non-determinism** — `Allocator.alloc()` captured R7 at call time;
   `Allocator.dispose()` dispatched to R7 at call time. Scope changes between alloc and free
   silently corrupted the wrong slab.
2. **Over-engineered defaults** — all allocations routed through MTIS even for short-lived
   bump-style use. Resource scope (FT-12) was the right pattern but bolted on as secondary API.
3. **Interface in the wrong library** — `sc_allocator_i` was defined in `sigma.memory` but
   consumed by `sigma.text` which links only `sigma.core.alloc`. Independence was illusory.

## Architecture

See [`docs/design.md`](docs/design.md) for the full specification.

**Short version:**
```
slab s  = Allocator.acquire(8 * 1024 * 1024);   // raw mmap region
bump_allocator a = Allocator.create_bump(s);     // typed controller; struct in SLB0
a->alloc(a, 1024);
frame f = a->frame_begin(a);
a->alloc(a, 512);
a->frame_end(a, f);    // roll back to just before the 512-byte alloc
a->reset(a, false);    // whole slab wiped, reusable
Allocator.release((sc_ctrl_base_s *)a);          // shutdown + deregister + munmap
```

## Building

See [`BUILDING.md`](BUILDING.md).

## History

Design evolution through v0.2.x is documented in [`../archive/sigma.mem_0.2/docs/design-chronicle.md`](../archive/sigma.mem_0.2/docs/design-chronicle.md).
