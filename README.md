# sigma.memory

**Version:** 0.2.0-alpha  
**Status:** Alpha Release

A lightweight C memory allocator with scope-based allocation and B-tree external metadata for the SigmaCore framework.

## Features

- **Zero configuration** - Auto-initializes before `main()`
- **Simple API** - `Allocator.alloc()` / `Allocator.dispose()`
- **B-tree metadata** - External allocation tracking, 100% page utilization
- **Dynamic growth** - 2KB initial NodePool, grows to 32KB+
- **Fast** - 1.5M-5.5M ops/sec, O(log n) operations

## Quick Start

```c
#include <sigma.memory/memory.h>

int main(void) {
    // Allocate 128 bytes
    object buffer = Allocator.alloc(128);
    
    // Use it...
    
    // Release
    Allocator.dispose(buffer);
    
    return 0;
}
```

## Build

```bash
# Build library
./cbuild lib

# Run tests
./ctest

# Run with valgrind
./ctest slab0 --valgrind
```

## Documentation

| Document | Description |
|----------|-------------|
| [USERS_GUIDE.md](docs/USERS_GUIDE.md) | How to use the library |
| [ROADMAP.md](docs/ROADMAP.md) | Backlog, priorities, version plans |
| [MEMORY_REFERENCE.md](docs/MEMORY_REFERENCE.md) | Technical architecture |
| [MEMORY_DESIGN.md](docs/MEMORY_DESIGN.md) | Design rationale |
| [BUILDING.md](BUILDING.md) | Build system details |

## Architecture

| Component | Size | Purpose |
|-----------|------|---------|
| **SYS0** | 4KB | Internal bootstrap allocator |
| **SLB0** | 64KB | User-space allocator (16 pages) |

## Requirements

- GCC or Clang (C11+)
- Linux with `mmap` support
- Dependencies: `sigma.core`, `sigma.collections`

## Roadmap

| Version | Features |
|---------|----------|
| **0.1.0** | ✅ SYS0 bootstrap, SLB0 allocator, 30 tests |
| 0.2.0 | Dynamic page growth, free block size tracking |
| 0.3.0 | User arenas (scope_table[2-15]) |
| 1.0.0 | Frame checkpoints, API stability |

## License

MIT License - See [LICENSE](LICENSE) for details.
