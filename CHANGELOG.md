# Changelog

All notable changes to Sigma.Memory are documented here.  
**Previous history (v0.1.0 – v0.2.5):** see [`../archive/sigma.mem_0.2/CHANGELOG.md`](../archive/sigma.mem_0.2/CHANGELOG.md)

---

## [0.3.0] - TBD

**Controller Model Rewrite**

Architecture fully redesigned. See [`docs/design.md`](docs/design.md) for the complete specification.

- `slab` type: raw mmap-backed memory region, no policy embedded
- `bump_allocator` (`sc_bump_ctrl_s *`): pure cursor bump, O(1) alloc, `reset`, frame snapshots
- `reclaim_allocator` (`sc_reclaim_ctrl_s *`): MTIS-backed, individual `free`, frame sequence-tag sweep
- Controller structs allocated from SLB0 (no separate bump pool in SYS0)
- Controller registry: `sc_ctrl_registry_s` embedded in SYS0; tracks up to `SC_MAX_CONTROLLERS` controller pointers
- R7 fixed permanently to SLB0 — scope stack removed
- `sc_allocator_i` interface definition moved to `sigma.core`
- Removed: `Allocator.Scope`, `Allocator.Arena`, `Allocator.Resource`, `Allocator.promote`, frame depth globals
- Retained: `Allocator.alloc / free / realloc` facade (dispatches to SLB0) for drop-in compat
