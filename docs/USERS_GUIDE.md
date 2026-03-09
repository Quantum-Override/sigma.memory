# SigmaCore Memory - User's Guide

**Version:** 0.2.3-realloc  
**Date:** March 9, 2026

---

## Overview

`sigma.memory` is a lightweight C memory allocator library with scope-based allocation and user-defined arenas. It provides:

- **Automatic initialization** via constructor (`__attribute__((constructor))`)
- **Simple API** with `Allocator.alloc()` and `Allocator.dispose()`
- **User arenas** for isolated allocation domains (up to 14 concurrent arenas)
- **Frame-based bulk deallocation** for temporary allocations
- **Zero configuration** required for basic use

---

## Requirements

### Build Tools
- GCC or Clang (C11 or later)
- Linux with `mmap` support
- Optional: Valgrind for memory checking

### Dependencies
- `sigma.core` (types.h)
- `sigma.test` (for test execution)

---

## Installation

### Build the Library

```bash
# Clone the repository
git clone https://github.com/Quantum-Override/sigma.memory.git
cd sigma.memory

# Build shared library
./cbuild lib

# Library output: bin/lib/libsigma.memory.so
```

### Install Headers

Copy headers to your include path:

```bash
sudo cp -r include/memory.h /usr/local/include/sigma.memory/
sudo cp bin/lib/libsigma.memory.so /usr/local/lib/
sudo ldconfig
```

---

## Quick Start

### Basic Usage

```c
#include <sigma.memory/memory.h>

int main(void) {
    // Memory system auto-initializes before main()
    
    // Allocate 128 bytes
    object buffer = Allocator.alloc(128);
    if (buffer == NULL) {
        // Handle allocation failure
        return 1;
    }
    
    // Use the memory...
    char *str = (char *)buffer;
    str[0] = 'H';
    str[1] = 'i';
    str[2] = '\0';
    
    // Release when done
    Allocator.dispose(buffer);
    
    return 0;
}
```

### Compile & Link

```bash
gcc -o myapp myapp.c -I/usr/local/include -lsigma.memory
```

---

## API Reference

### Core Types

```c
typedef void *object;    // Generic pointer (from sigma.core/types.h)
typedef size_t usize;    // Unsigned size type
```

### Allocator Interface

| Function | Description |
|----------|-------------|
| `Allocator.alloc(size)` | Allocate `size` bytes from current scope |
| `Allocator.dispose(ptr)` | Release memory back to its scope |
| `Allocator.realloc(ptr, size)` | Resize allocation: shrinks in-place, grows via alloc+copy+dispose; `NULL` ptr → alloc; `size=0` → dispose |

### Scope Interface (Advanced)

| Function | Description |
|----------|-------------|
| `Allocator.Scope.current()` | Get pointer to current scope |
| `Allocator.Scope.set(scope)` | Switch active scope |
| `Allocator.Scope.config(scope, mask)` | Query scope policy/flags |
| `Allocator.Scope.alloc(scope, size)` | Allocate from specific scope |
| `Allocator.Scope.dispose(scope, ptr)` | Dispose to specific scope |

### Example: Query Current Scope

```c
#include <sigma.memory/memory.h>

void show_scope_info(void) {
    void *scope = Allocator.Scope.current();
    
    sbyte policy = Allocator.Scope.config(scope, SCOPE_POLICY);
    sbyte flags = Allocator.Scope.config(scope, SCOPE_FLAG);
    
    printf("Policy: %d, Flags: 0x%02x\n", policy, flags);
}
```

---

## Frame API (v0.2.1+)

Frames provide arena-style bulk deallocation for temporary allocations. Perfect for testing, temporary buffers, and phase-based computation.

### Frame Operations

| Function | Description |
|----------|-------------|
| `Allocator.frame_begin()` | Create new frame, returns opaque handle |
| `Allocator.frame_end(frame)` | Deallocate all frame allocations |
| `Allocator.frame_depth()` | Current frame nesting depth (0-16) |
| `Allocator.frame_allocated(frame)` | Bytes allocated in frame |

### Example: Frame-Based Testing

```c
#include <sigma.memory/memory.h>

void test_with_frames(void) {
    // Create frame for temporary allocations
    frame f = Allocator.frame_begin();
    if (!f) {
        // Max nesting depth (16) reached
        return;
    }
    
    // All allocations use frame's bump allocator (O(1))
    object buffer1 = Allocator.alloc(128);
    object buffer2 = Allocator.alloc(256);
    object buffer3 = Allocator.alloc(512);
    
    // Use buffers for test operations...
    
    // Bulk deallocate everything (O(k) where k = chunk count)
    Allocator.frame_end(f);
    // buffer1, buffer2, buffer3 now invalid
}
```

### Example: Nested Frames

```c
void nested_frames(void) {
    frame outer = Allocator.frame_begin();
    
    object a = Allocator.alloc(64);  // From outer frame
    
    frame inner = Allocator.frame_begin();
    object b = Allocator.alloc(128); // From inner frame
    
    // Check nesting
    usize depth = Allocator.frame_depth();  // Returns 2
    
    // End inner frame first (LIFO ordering)
    Allocator.frame_end(inner);  // Deallocates b
    
    object c = Allocator.alloc(32);  // Still in outer frame
    
    Allocator.frame_end(outer);  // Deallocates a, c
}
```

### Frame Characteristics

| Property | Value | Notes |
|----------|-------|-------|
| Chunk size | 4 KB | Automatic chaining when full |
| Max nesting | 16 levels | LIFO stack limit |
| Allocation speed | O(1) | Bump pointer, no search |
| Deallocation speed | O(k) | k = number of 4KB chunks |
| Overhead | 24 bytes | Per chunk (sc_node) |

### Frame Limitations

- **LIFO ordering required**: Must end inner frames before outer
- **No partial deallocation**: frame_end() deallocates everything
- **Chunk overhead**: Small allocations (< 4KB) waste no space, larger ones chain chunks
- **Not thread-safe**: Single-threaded only (v0.2.1)

---

## Scope Policies

| Policy | Value | Description |
|--------|-------|-------------|
| `SCOPE_POLICY_RECLAIMING` | 0 | First-fit with block reuse (SYS0 only) |
| `SCOPE_POLICY_DYNAMIC` | 1 | Auto-grows by chaining pages |
| `SCOPE_POLICY_FIXED` | 2 | Pre-allocated; returns NULL when exhausted |

## Scope Flags

| Flag | Bit | Description |
|------|-----|-------------|
| `SCOPE_FLAG_PROTECTED` | 0x01 | Cannot be disposed |
| `SCOPE_FLAG_PINNED` | 0x02 | Cannot be moved or swapped |
| `SCOPE_FLAG_SECURE` | 0x04 | Zero memory on dispose |

---

## Memory Limits

| Resource | Size | Notes |
|----------|------|-------|
| User memory (SLB0) | 64 KB | 16 pages × 4KB |
| Max allocation | ~4032 bytes | Single page minus sentinel |
| Minimum allocation | 16 bytes | Aligned to `kAlign` |

> **Note:** Dynamic page growth is planned for v0.2. Current version has fixed 64KB pool.

---

## Testing

### Run All Tests

```bash
./ctest
```

### Run Specific Test Suite

```bash
./ctest bootstrap    # SYS0 bootstrap tests
./ctest slab0        # User allocator tests
```

### Memory Leak Detection

```bash
./ctest slab0 --valgrind
```

---

## Troubleshooting

### Allocation Returns NULL

1. **Requested size exceeds page capacity** (~4032 bytes max)
2. **All pages exhausted** (64KB limit reached)
3. **Zero-size request** (intentional)

### Valgrind Reports "still reachable"

If you see ~2KB "still reachable" from sigtest framework, this is expected and not a leak in sigma.memory.

---

## Version History

| Version | Date | Changes |
|---------|------|------|
| 0.1.0 | 2026-01-29 | Initial release: SYS0 bootstrap, SLB0 user allocator |
| 0.2.0 | 2026-02-12 | B-tree external metadata, dynamic NodePool growth |
| 0.2.1 | 2026-03-08 | Frame support (chunk-based bump allocator) |
| 0.2.2 | 2026-03-08 | User arenas (14 concurrent arenas, simple bump allocation) |
| **0.2.3** | **2026-03-09** | **`Allocator.realloc`, SLB0 dynamic page release, skip list correctness fix** |

---

## Arena API (v0.2.2+)

User arenas provide isolated allocation domains with simple bump allocation. Perfect for subsystems, plugins, or temporary workspaces that need clean separation.

### Arena Operations

| Function | Description |
|----------|-------------|
| `Allocator.create_arena(name, policy)` | Create new arena, returns scope handle |
| `Allocator.dispose_arena(scope)` | Dispose entire arena and all allocations |
| `Allocator.Scope.alloc(scope, size)` | Allocate from specific arena |

### Example: Basic Arena Usage

```c
#include <sigma.memory/memory.h>

void example_arena_basic(void) {
    // Create arena for temporary work
    scope arena = Allocator.create_arena("workspace", SCOPE_POLICY_DYNAMIC);
    if (!arena) {
        // Max 14 arenas reached
        return;
    }
    
    // Allocate from arena
    object buffer1 = Allocator.Scope.alloc(arena, 1024);
    object buffer2 = Allocator.Scope.alloc(arena, 2048);
    
    // Use buffers...
    memset(buffer1, 0, 1024);
    memset(buffer2, 0, 2048);
    
    // Dispose entire arena (all allocations freed)
    Allocator.dispose_arena(arena);
    // buffer1, buffer2 now invalid
}
```

### Example: Arena as Current Scope

```c
void example_arena_as_current(void) {
    scope arena = Allocator.create_arena("current", SCOPE_POLICY_RECLAIMING);
    
    // Get SLB0 reference to restore later
    scope slb0 = Memory.get_scope(1);
    
    // Switch to arena as current scope
    Allocator.Scope.set(arena);
    
    // Allocator.alloc() now uses arena
    object ptr1 = Allocator.alloc(256);
    object ptr2 = Allocator.alloc(512);
    
    // Switch back to SLB0
    Allocator.Scope.set(slb0);
    
    // Cleanup
    Allocator.dispose_arena(arena);
}
```

### Example: Multiple Isolated Arenas

```c
void example_multiple_arenas(void) {
    // Create arenas for different subsystems
    scope audio_arena = Allocator.create_arena("audio", SCOPE_POLICY_FIXED);
    scope render_arena = Allocator.create_arena("render", SCOPE_POLICY_DYNAMIC);
    scope network_arena = Allocator.create_arena("network", SCOPE_POLICY_RECLAIMING);
    
    // Each arena has independent allocation space
    object audio_buf = Allocator.Scope.alloc(audio_arena, 4096);
    object render_buf = Allocator.Scope.alloc(render_arena, 8192);
    object net_buf = Allocator.Scope.alloc(network_arena, 1024);
    
    // Dispose one arena doesn't affect others
    Allocator.dispose_arena(audio_arena);
    
    // render_buf and net_buf still valid
    // ...
    
    // Cleanup
    Allocator.dispose_arena(render_arena);
    Allocator.dispose_arena(network_arena);
}
```

### Arena Characteristics

| Property | Value | Notes |
|----------|-------|-------|
| Max arenas | 14 concurrent | Slots 2-15 in scope_table |
| Page size | 8 KB | Automatic page chaining |
| Allocation speed | O(1) | Simple bump pointer |
| Deallocation | Arena-wide only | No individual free |
| Name length | 15 chars + null | Truncated if longer |
| Overhead | 32 bytes/page | Page sentinel metadata |

### Arena Policies

| Policy | Description | Use Case |
|--------|-------------|----------|
| `SCOPE_POLICY_RECLAIMING` | Future: block reuse | General purpose (currently same as DYNAMIC) |
| `SCOPE_POLICY_DYNAMIC` | Auto-grows, unlimited pages | Long-lived workspaces |
| `SCOPE_POLICY_FIXED` | Single page, fails when full | Size-bounded operations |

### Arena Limitations

- **No individual deallocation**: Must dispose entire arena
- **14 arena limit**: System maximum (SLB1-14)
- **Single-threaded**: Not thread-safe in v0.2.3
- **Linear allocation only**: No free list or reuse within arena

---

## Roadmap

| Version | Features |
|---------|----------|
| 0.2.2 | ✅ User arenas (14 concurrent, simple bump allocation) |
| **0.2.3** | ✅ **`Allocator.realloc`, dynamic page release, skip list fix** |
| 0.2.4 | Arena frames (frame support within arenas) |
| 0.3.0 | Thread-safety, advanced policies |
| 1.0.0 | Full API stability, production-ready |

---

## See Also

- [MEMORY_REFERENCE.md](MEMORY_REFERENCE.md) - Technical architecture details
- [MEMORY_DESIGN.md](MEMORY_DESIGN.md) - Design rationale and decisions
- [BUILDING.md](../BUILDING.md) - Build system documentation
