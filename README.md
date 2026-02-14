# sigma.memory

**Version:** 0.2.0-alpha  
**Status:** Alpha Release

A high-performance C memory allocator with B-tree external metadata architecture designed for systems programming where you need fast, predictable allocations without libc dependencies.

## What It Is

**Sigma.Memory** is:
- ✅ A **malloc/free replacement** for bare-metal and systems programming
- ✅ A **scope-based allocator** with arena/frame support (v0.2.1+)
- ✅ An **OS-level memory manager** with 100% page utilization
- ✅ A **B-tree metadata architecture** for O(log n) allocation/deallocation
- ✅ **Valgrind-clean** with zero memory leaks

## What It Is NOT

**Sigma.Memory** is not:
- ❌ A general-purpose malloc() wrapper (use system malloc for that)
- ❌ A garbage collector (manual dispose required)
- ❌ Thread-safe yet (v0.2.1 designs thread-friendly hooks for Sigma.Tasking)
- ❌ A debugging allocator (no guard pages, poisoning - yet)

## Why Use It?

**Benefits:**
- 🚀 **Fast**: 1.5M-5.5M ops/sec, 2-3x faster than naive implementations
- 📦 **Small**: 2KB initial footprint, grows dynamically only when needed
- 🎯 **Predictable**: O(log n) operations, no hidden costs
- 🔬 **Testable**: 45 passing tests, comprehensive validation suite
- 🧩 **Modular**: Clean C11 interface, works with SigmaCore or standalone
- 💾 **Efficient**: External metadata = 100% of allocated pages are usable payload

**Use Cases:**
- Embedded systems with limited memory
- Real-time applications needing predictable allocation
- Testing frameworks (arena-based test isolation)
- Game engines (frame-based memory management)
- Systems where libc malloc isn't available

## Features (v0.2.0-alpha)

### Core Architecture
- **B-Tree External Metadata** - Allocation tracking outside user pages (100% utilization)
- **Dynamic Node Pool** - Starts at 2KB, grows to 4→8→16→32KB as needed
- **24-byte Nodes** - Cache-aligned, efficient tree structures
- **Register Machine Model** - Stack-based operations, minimal ABI overhead

### Performance & Reliability
- **High Throughput** - 1.5M-5.5M operations/sec depending on workload
- **O(log n) Operations** - Predictable performance characteristics
- **Valgrind Clean** - Zero leaks, zero errors in 45 comprehensive tests
- **Memory Efficient** - Critical bugfix: splits reuse nodes (40 iterations stable)

### Developer Experience
- **Zero Configuration** - Auto-initializes before `main()`
- **Simple API** - `Allocator.alloc()` / `Allocator.dispose()`
- **TDD Validated** - 45 tests covering bootstrap, btree, integration, stress, performance
- **Well Documented** - Architecture guide, API reference, user's guide

## Quick Start

### Installation

```bash
# Clone the repository
git clone https://github.com/Quantum-Override/sigma.memory.git
cd sigma.memory

# Build and package
cbuild          # Compile library
cpub memory     # Package to /usr/local/packages/sigma.memory.o
```

### Basic Usage

```c
#include <sigma.memory/memory.h>

int main(void) {
    // Allocate 128 bytes
    object buffer = Allocator.alloc(128);
    if (!buffer) {
        // Handle allocation failure
        return -1;
    }
    
    // Use buffer...
    memset(buffer, 0, 128);
    
    // Release when done
    Allocator.dispose(buffer);
    
    return 0;
}
```

### Linking Your Project

```bash
# Using packaged object
gcc myapp.c /usr/local/packages/sigma.memory.o -I/usr/local/include/sigma.memory -o myapp

# Or using source directly
gcc myapp.c sigma.memory/build/*.o -Isigma.memory/include -o myapp
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
| **SYS0** | 8KB | Internal bootstrap allocator (6.6KB allocable) |
| **NodePool** | 2KB → 32KB | B-tree node storage (grows dynamically) |
| **SLB0** | 64KB | User-space allocator (16 × 4KB pages) |
| **Nodes** | 24 bytes | B-tree allocation descriptors |

**Key Design Decisions:**
- External metadata = user pages are 100% payload
- First-fit allocation with log2 size hints
- Block coalescing on free (immediate, not deferred)
- No tree rebalancing in v0.2.0 (validated unnecessary for typical workloads)

## Requirements

- GCC or Clang (C11+)
- Linux with `mmap` support
- Dependencies: `sigma.core`, `sigma.collections`

## Roadmap

| Version | Status | Features |
|---------|--------|----------|
| **0.1.0** | ✅ Released | SYS0 bootstrap, SLB0 allocator, 30 tests |
| **0.2.0-alpha** | ✅ Released | B-tree metadata, dynamic growth, 45 tests, critical bugfixes |
| 0.2.1 | 🚧 Planned | Frame/arena support, thread-friendly design |
| 0.3.0 | 📋 Future | User arenas (SLB1-14), thread-safety with Sigma.Tasking |
| 1.0.0 | 📋 Future | Production hardening, API stability, full frame checkpoints |

## License

MIT License - See [LICENSE](LICENSE) for details.
