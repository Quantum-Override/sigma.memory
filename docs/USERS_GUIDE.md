# SigmaCore Memory - User's Guide

**Version:** 0.1.0  
**Date:** January 29, 2026

---

## Overview

`sigma.memory` is a lightweight C memory allocator library with scope-based allocation. It provides:

- **Automatic initialization** via constructor (`__attribute__((constructor))`)
- **Simple API** with `Allocator.alloc()` and `Allocator.dispose()`
- **64KB user memory** (16 × 4KB pages) ready at startup
- **Zero configuration** required for basic use

---

## Requirements

### Build Tools
- GCC or Clang (C11 or later)
- Linux with `mmap` support
- Optional: Valgrind for memory checking

### Dependencies
- `sigma.core` (types.h)
- `sigma.collections` (parray, slotarray)

---

## Installation

### Build the Library

```bash
# Clone the repository
git clone https://github.com/sigmacore/sigma.memory.git
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
|---------|------|---------|
| 0.1.0 | 2026-01-29 | Initial release: SYS0 bootstrap, SLB0 user allocator |

---

## Roadmap

| Version | Features |
|---------|----------|
| 0.2.0 | Dynamic page growth, free block size tracking |
| 0.3.0 | User arenas (scope_table[2-15]) |
| 1.0.0 | Frame checkpoints, full API stability |

---

## See Also

- [MEMORY_REFERENCE.md](MEMORY_REFERENCE.md) - Technical architecture details
- [MEMORY_DESIGN.md](MEMORY_DESIGN.md) - Design rationale and decisions
- [BUILDING.md](../BUILDING.md) - Build system documentation
