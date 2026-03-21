# Sigma.Memory v0.2.2 - Integration Guide for Sigma.Test & Anvil

> **⚠ v0.3.0 NOTE — UPDATE NEEDED**  
> This integration guide covers the v0.2.x `Allocator.Arena` / `Allocator.Resource` API.  
> After 0.3.0 lands, rewrite this guide around the Controller Model API.  
> Key integration changes: sigma.text links `sigma.core` for `sc_allocator_i` (no longer `sigma.memory`);  
> Anvil's `Allocator.Resource` usage maps to `bump_allocator` with `Allocator.acquire + create_bump`;  
> sigma.test bootstrap uses `sigma.core.alloc` (malloc-backed reclaim controller).

**Date:** March 8, 2026  
**Status:** Archived — v0.2.x integration guide only

---

## What You're Getting

**Multi-Arena Memory Management** with:
- ✅ Up to 14 user arenas (SLB1-SLB14)
- ✅ Per-arena allocation/disposal
- ✅ Per-arena frame support (bulk deallocation)
- ✅ Arena create/destroy lifecycle
- ✅ Automatic NodePool growth (no more exhaustion)
- ✅ Thread-friendly (hooks for future Sigma.Tasking integration)

---

## Why This Matters

### For Sigma.Test
**Problem:** Test memory leaks contaminate subsequent tests  
**Solution:** Per-test arena isolation

```c
void run_test(test_func func) {
    scope test_arena = Arena.create("test", POLICY_RECLAIMING);
    frame f = Arena.frame_begin(test_arena);
    
    func();  // Test runs in isolated arena
    
    Arena.frame_end(test_arena, f);
    Arena.destroy(test_arena);  // All test allocations gone
}
```

**Benefits:**
- Memory leaks don't cross test boundaries
- Fast teardown (destroy arena vs individual dispose calls)
- Memory tracking per test (identify heavy tests)

---

### For Anvil
**Problem:** Compiler phases interfere, hard to profile memory usage  
**Solution:** Per-module or per-phase arenas

```c
// Per-phase isolation
scope parse_arena = Arena.create("parser", POLICY_RECLAIMING);
scope opt_arena = Arena.create("optimizer", POLICY_RECLAIMING);

// Parse phase
Arena.set_current(parse_arena);
AST *ast = parse(input);

// Optimization phase
Arena.set_current(opt_arena);
AST *optimized = optimize(ast);

// Profile memory usage
printf("Parser used: %zu bytes\n", Arena.allocated(parse_arena));
printf("Optimizer used: %zu bytes\n", Arena.allocated(opt_arena));

// Cleanup
Arena.destroy(parse_arena);
Arena.destroy(opt_arena);
```

**Benefits:**
- Isolate compiler phases
- Memory profiling per phase
- Clean phase boundaries
- No cross-phase contamination

---

## API Preview

### Arena Management
```c
// Create arena (name max 15 chars)
scope Arena.create(const char *name, sbyte policy);

// Destroy arena (frees ALL allocations)
void Arena.destroy(scope s);

// Set current arena (thread-local in future)
bool Arena.set_current(scope s);
scope Arena.current(void);
```

### Allocation (Scoped)
```c
// Allocate in specific arena
object Arena.alloc(scope s, usize size);
void Arena.dispose(scope s, object ptr);

// Allocate in current arena (convenience)
object Allocator.alloc(usize size);  // Uses current scope
void Allocator.dispose(object ptr);
```

### Frames (Scoped)
```c
// Begin frame in specific arena
frame Arena.frame_begin(scope s);
integer Arena.frame_end(scope s, frame f);

// Frame in current arena (convenience)
frame Allocator.frame_begin(void);  // Uses current scope
integer Allocator.frame_end(frame f);
```

### Introspection
```c
// Arena stats
usize Arena.page_count(scope s);
usize Arena.allocated(scope s);
const char* Arena.name(scope s);
```

---

## Policies (v0.2.2)

**POLICY_RECLAIMING** (Only policy in v0.2.2)
- B-Tree backed allocation tracking
- O(log n) allocation/disposal
- Automatic coalescing on free
- Same behavior as current SLB0

**Note:** POLICY_BUMP and POLICY_FIXED deferred based on feedback. Do you need them?

---

## Thread-Friendly (NOT Thread-Safe!)

**Important:** v0.2.2 is **single-threaded only**.

**Thread-Friendly Hooks** (for future Sigma.Tasking):
```c
typedef struct sc_memory_hooks {
    void (*before_scope_alloc)(scope s);
    void (*after_scope_alloc)(scope s);
    void (*before_scope_create)(void);
    void (*after_scope_create)(void);
} sc_memory_hooks;

// Future: Sigma.Tasking will implement coordination
void Memory.set_hooks(const sc_memory_hooks *hooks);
```

**Philosophy:** Sigma.Memory is a kernel component. Sigma.Tasking manages threading. We provide hooks, not locks.

---

## Questions for Dog-Fooding

### Sigma.Test Team
1. **Granularity:** Per-test arenas? Or per-suite arenas?
2. **Nesting:** Do tests create sub-arenas?
3. **Count:** How many concurrent test arenas do you expect?
4. **Teardown:** Do you need arena callbacks for cleanup?
5. **Stats:** What memory metrics do you need? (max usage, alloc count, etc.)

### Anvil Team
1. **Granularity:** Per-module arenas? Or per-phase arenas?
2. **Count:** How many compiler phases? (sizing question)
3. **Policies:** Is POLICY_RECLAIMING sufficient? Or need POLICY_BUMP?
4. **Profiling:** Do you need per-arena callbacks for memory tracking?
5. **Data flow:** How do you pass data between phases with different arenas?

### Both Teams
6. **API Confusion:** What's awkward about the API?
7. **Missing Features:** What did you expect but didn't get?
8. **Performance:** Any bottlenecks or slowdowns?
9. **Bugs:** What broke? What's surprising behavior?
10. **Thread Model:** How will you eventually use threads? (informs Sigma.Tasking design)

---

## Integration Timeline

**Week 1 (Mar 8-15):** Sigma.Memory implements NodePool growth + basic arenas  
**Week 2 (Mar 15-22):** Sigma.Memory implements per-arena frames + hooks  
**Week 3 (Mar 22-29):** Sigma.Memory validation + release prep  

**Week 4 (Mar 29-Apr 5):** **YOU integrate** into Sigma.Test and Anvil  
**Week 5 (Apr 5-12):** **Feedback loop** - report issues, requests  
**Week 6 (Apr 12-19):** Quick fixes based on critical feedback  

**April 2026:** v0.3.0 planning based on your real-world usage

---

## What We Need From You

### During Integration (Week 4-5)
- Try the API, break things
- Report confusing parts
- Identify missing features
- Measure performance impact

### After Integration (Week 6+)
- What works well?
- What's awkward?
- What's missing?
- Performance acceptable?

### Long-Term
- How does it fit your architecture?
- How will threading work? (informs Sigma.Tasking)
- What policies do you actually need?
- What introspection is useful?

---

## Success Criteria

**For Sigma.Test:**
✅ Can run test suite with per-test arenas  
✅ Memory leaks don't cross test boundaries  
✅ Teardown is fast  
✅ Memory tracking works  

**For Anvil:**
✅ Can allocate per compilation phase  
✅ Memory profiling shows phase usage  
✅ Phase isolation works  
✅ Cleanup is correct  

**For Sigma.Memory:**
✅ Real-world usage validates design  
✅ Identifies missing features  
✅ Informs v0.3.0 priorities  
✅ Proves arena value proposition  

---

## Contact

**Questions during development:** Ping Sigma.Memory team  
**Issues during integration:** File GitHub issues  
**Feedback after dog-fooding:** Weekly sync meetings  

**Let's build something great together!** 🚀

---

**Next Steps:**
1. Read this guide
2. Think about your use cases
3. Prepare your integration questions
4. We deliver v0.2.2 (March 29)
5. You integrate (April 1-5)
6. We iterate based on feedback
