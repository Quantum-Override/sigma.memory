# Bash Build System (BBS) v0.2.0

A simple, Makefile-free build system using bash scripts and configuration files. Designed for C projects, easy to configure without JSON/TOML/YAML.

## Overview

This build system consists of:
- `config.sh`: Configuration file with build variables.
- `cbuild`: Main build script that sources `config.sh` and handles targets.
- `ctest`: Test runner for individual tests.
- `cpack`: Packager for creating fat objects from configured source lists.
- `cpub`: Publishing script for installing packaged artifacts (optional).

## Configuration (`config.sh`)

Edit `config.sh` to customize your build. It contains shell variables for compiler, flags, directories, etc.

### Variables

- `CC`: Compiler (default: `gcc`)
- `STD`: C standard (default: `c2x`)
- `CFLAGS`: Compilation flags for library sources
- `TST_CFLAGS`: Compilation flags for test sources (includes `CFLAGS` + test-specific flags)
- `LDFLAGS`: Linker flags for shared library
- `TST_LDFLAGS`: Linker flags for test executables
- `SRC_DIR`: Directory for source files (default: `src`)
- `BUILD_DIR`: Directory for build artifacts (default: `build`)
- `BIN_DIR`: Directory for binaries (default: `bin`)
- `LIB_DIR`: Directory for libraries (default: `$BIN_DIR/lib`)
- `TEST_DIR`: Directory for test sources (default: `test`)
- `TST_BUILD_DIR`: Directory for test build artifacts (default: `$BUILD_DIR/test`)
- `PACKAGES`: Associative array of package definitions (e.g., `PACKAGES["collection"]="sigma.collections | array_base collections list parray farray slotarray"`)
- `REQUIRES`: Array of required package names (e.g., `REQUIRES=("sigma.collections" "other.package")`)
- `VALGRIND_ENABLED`: Enable valgrind support (default: `true` if valgrind is installed)
- `VALGRIND_OPTS`: Valgrind command-line options (default: `--leak-check=full --show-leak-kinds=all --track-origins=yes --verbose`)
- `ASAN_ENABLED`: Enable AddressSanitizer compilation (default: `false`)
- `ASAN_OPTIONS`: AddressSanitizer runtime options (default: `detect_leaks=1:detect_stack_use_after_return=1:detect_invalid_pointer_pairs=1`)
- `BUILD_TARGETS`: Associative array of custom build targets (e.g., `BUILD_TARGETS["all"]="build_lib"`)
- `TEST_CONFIGS`: Associative array of test-specific configurations (e.g., `TEST_CONFIGS["memory"]="with_proto_objects"`)
- `TEST_COMPILE_FLAGS`: Associative array of test-specific preprocessor flags (e.g., `TEST_COMPILE_FLAGS["bootstrap"]="TEST_BOOTSTRAP_ONLY"`)

### Example Customization

```bash
# Use clang instead of gcc
CC=clang

# Add optimization
CFLAGS="-Wall -Wextra -O2 -fPIC -std=$STD -I./include"

# Custom include path
CFLAGS="$CFLAGS -I./custom/include"
```

## Memory Checking

The build system supports two memory checking tools:

### Valgrind
Runtime memory checker that detects:
- Memory leaks
- Invalid memory access
- Use of uninitialized memory
- Double frees

**Setup:**
```bash
# Enable valgrind in config.sh
VALGRIND_ENABLED=true

# Run tests with valgrind
./ctest memory --valgrind
```

### AddressSanitizer (ASAN)
Compile-time instrumentation that detects:
- Buffer overflows/underflows
- Use-after-free
- Memory leaks
- Invalid pointer operations

**Setup:**
```bash
# Enable ASAN in config.sh
ASAN_ENABLED=true

# Rebuild and run tests
./cbuild clean_all
./cbuild all
./ctest memory --asan
```

**Note:** ASAN requires recompilation with sanitizer flags, while valgrind works on existing binaries.

## Custom Build Targets

The build system supports configurable build targets defined in `config.sh`. Each project can customize the available targets and their behaviors.

### Defining Custom Targets

```bash
# In config.sh
declare -A BUILD_TARGETS=(
    ["all"]="build_lib"
    ["compile"]="compile_only"
    ["clean"]="clean"
    ["clean_all"]="clean_all"
    ["install"]="build_lib && install_lib"
    ["test"]="run_all_tests"
    ["root"]="show_project_info"
)

# Add custom targets
BUILD_TARGETS["docs"]="generate_docs"
BUILD_TARGETS["package"]="build_lib && create_package"
```

### Test Configurations

Test linking behavior can be customized per test:

```bash
# In config.sh
declare -A TEST_CONFIGS=(
    ["memory"]="with_proto_objects"  # Links with prototyping objects
    ["farray"]="standard"            # Standard linking
    ["custom"]="custom_linker"       # Custom linking strategy
)
```

### Test-Specific Compilation Flags

Individual tests can have preprocessor flags applied during compilation. This is useful for:
- Feature isolation (e.g., testing minimal bootstrap initialization)
- Conditional compilation paths
- Test-specific behavior switches

```bash
# In config.sh
declare -A TEST_COMPILE_FLAGS=(
    ["bootstrap"]="TEST_BOOTSTRAP_ONLY"           # Single flag
    ["isolation"]="TEST_MODE SKIP_INIT DEBUG"     # Multiple space-separated flags
)
```

**How it works:**
- Flags are specified as space-separated values (no `-D` prefix needed)
- `ctest` automatically prefixes each flag with `-D` during compilation
- Flags are applied to **both** library object compilation and test object compilation
- This allows conditional code paths in your source files using `#ifdef`, `#ifndef`, etc.

**Example usage in source code:**

```c
// In memory.c
__attribute__((constructor)) void init_memory_system(void) {
    sys0 = (void *)sys_page;
    
    #ifndef TEST_BOOTSTRAP_ONLY
        // Full initialization with user memory (SLB0)
        scope slb0 = (scope)sys0_alloc(sizeof(sc_scope));
        // ... full setup ...
    #elifdef TEST_BOOTSTRAP_ONLY
        // Minimal bootstrap - only SYS0 initialization
        printf("Memory system initialized in TEST_BOOTSTRAP_ONLY mode\n");
    #endif
}
```

Then run with: `ctest bootstrap` (automatically applies `-DTEST_BOOTSTRAP_ONLY`)

## Usage (`cbuild`)

Make `cbuild` executable: `chmod +x cbuild`

Run with: `./cbuild [target]`

### Targets

- `all` (default): Build the shared library from sources in `$SRC_DIR/*.c`
- `compile`: Compile sources to object files only (no linking)
- `clean`: Remove object files from build directory (including subdirectories)
- `clean_all`: Remove all build artifacts and binaries (preserves directories)
- `install`: Build library and install to system (`/usr/lib/`, `/usr/include/`)
- `root`: Display project information (verifies local `config.sh` usage)

### Examples

```bash
# Build library
./cbuild

# Compile only (object files)
./cbuild compile

# Clean build
./cbuild clean

# Show project info (verify config.sh usage)
./cbuild root

# Install library
sudo ./cbuild install
```

## Testing (`ctest`)

Make `ctest` executable: `chmod +x ctest`

Run with: `./ctest <test_name> [options]`

This builds the necessary library objects, compiles the test source (`$TEST_DIR/test_<test_name>.c`), links the test executable, and runs it.

### Options

- `--valgrind`: Run the test with valgrind for memory leak detection (requires `VALGRIND_ENABLED=true` in config.sh)
- `--asan`: Run the test with AddressSanitizer (requires `ASAN_ENABLED=true` in config.sh)
- `--help`, `-h`: Show help message

### Examples

```bash
# Run memory test normally
./ctest memory

# Run memory test with valgrind
./ctest memory --valgrind

# Run arrays test with AddressSanitizer
./ctest arrays --asan

# Show help
./ctest --help
```

## Packaging (`cpack`)

Make `cpack` executable: `chmod +x cpack`

Run with: `./cpack <bundle_name>`

This packages objects listed in the `PACKAGES` array from `config.sh` into the specified output file.

### Bundle Configuration Format

The `PACKAGES` associative array defines package recipes with the format:
```
["bundle_name"]="output_filename | source1 source2 source3"
```

- **bundle_name**: The name used when calling `./cpack bundle_name`
- **output_filename**: The name of the resulting bundled object file (without .o extension)
- **source1 source2 source3**: Space-separated list of source names (without .c extension)

**Example:**
```bash
declare -A PACKAGES=(
    ["collection"]="sigma.collections | array_base collections list parray farray slotarray"
    ["utils"]="my_utils | string_helper math_helper file_helper"
)
```

This creates:
- `./cpack collection` → `build/sigma.collections.o` (packaging array_base.o, collections.o, etc.)
- `./cpack utils` → `build/my_utils.o` (packaging string_helper.o, math_helper.o, etc.)

### Examples

```bash
# Package collection objects
./cpack collection

# Package utility objects
./cpack utils
```

## Publishing (`cpub`)

After packaging with `cpack`, use `cpub` to install the packaged artifacts system-wide.

Run with: `./cpub [prefix]`

This copies headers from `package/include/` to `$PREFIX/include/` and bundled objects from `package/` to `$PREFIX/packages/`.

### Publishing Workflow

1. Configure your package in `config.sh` PACKAGES array
2. Run `cpack <bundle_name>` to create `package/` with bundled objects and headers
3. Run `./cpub` to install to system directories
4. Other projects can now use your package via REQUIRES

## Package Dependencies (`REQUIRES`)

The build system supports declaring dependencies on pre-compiled packages stored as `.o` files. Required packages are automatically linked into all builds (libraries and tests).

### Package Dependency Configuration

Declare required packages in `config.sh`:

```bash
# Required packages (automatically linked)
REQUIRES=("sigma.collections" "other.package")
```

### How It Works

1. **Package Location**: Packages are expected as `.o` files in `/usr/local/packages/`
2. **Automatic Linking**: Required packages are included in all linking operations
3. **Build Validation**: Builds fail if required packages are missing
4. **Global Scope**: Dependencies are available for all build targets

### Example

If your project requires `sigma.collections`, add to `config.sh`:

```bash
REQUIRES=("sigma.collections")
```

The build system will:
- Check for `/usr/local/packages/sigma.collections.o`
- Include it in library linking: `gcc objects... sigma.collections.o -o lib...`
- Include it in test linking: `gcc test.o objects... sigma.collections.o -o test_exe`

### Package Installation

Packages should be installed to `/usr/local/packages/` by external means:
- `cpack` to bundle and package artifacts
- `cpub` to install packaged artifacts system-wide
- Manual copying
- Package managers
- Build system extensions

The build scripts only consume installed packages, they don't manage package installation.

### Publishing Workflow

For projects that provide packages:

1. Run `cpack <bundle_name>` to create bundled objects and copy headers to `package/`
2. Run `./cpub` to install from `package/` to system directories
3. Other projects can then use the package via REQUIRES

## Project Structure

Assumed directory layout:
```
project/
├── config.sh          # Configuration
├── cbuild             # Build script
├── ctest              # Test script
├── cpack              # Packaging script
├── cpub               # Publishing script (optional)
├── src/               # Source files (*.c)
├── include/           # Headers
├── test/              # Test files (*.c)
├── build/             # Generated objects
├── package/           # Packaged artifacts (created by cpack)
└── bin/lib/           # Output library
```

## Dependencies

- Bash shell
- GCC/Clang compiler
- For tests: `sigmatest` library (adjust `TST_LDFLAGS` if different)

## Advantages over Makefiles

- Readable bash syntax (no tabs, cryptic rules)
- Simple configuration via shell variables
- Easy to debug and modify
- No complex build system overhead

## Troubleshooting

- **No sources found**: Ensure files exist in `$SRC_DIR/*.c`
- **Compilation errors**: Check `CFLAGS` and source code
- **Test failures**: Verify test framework installation and `TST_LDFLAGS`
- **Permission denied**: Run `chmod +x cbuild ctest cpack cpub`
- **Scripts not found**: If installed to `/usr/local/scripts/`, ensure it's in your PATH or use full paths

## System Installation

The build scripts can be installed system-wide for use across projects:

```bash
# Install to /usr/local/scripts/
sudo mkdir -p /usr/local/scripts
sudo cp cbuild cpack ctest cpub /usr/local/scripts/

# Add to PATH permanently (for bash users)
echo 'export PATH="$PATH:/usr/local/scripts"' >> ~/.bashrc

# For zsh users, use ~/.zshrc instead:
# echo 'export PATH="$PATH:/usr/local/scripts"' >> ~/.zshrc

# Apply the PATH change immediately
source ~/.bashrc  # or source ~/.zshrc for zsh users
```

**Note:** `config.sh` is project-specific and should be created in each project's root directory, not installed system-wide.

Then use from any C project directory:
```bash
cbuild all        # Build library
cbuild test       # Run tests  
cbuild install    # Install system-wide
```

## Project Setup

For each new C project, create a `config.sh` file in the project root:

```bash
# Create basic config.sh for a new project
cat > config.sh << 'EOF'
# Configuration for MyProject
CC=gcc
STD=c2x
CFLAGS="-Wall -Wextra -g -fPIC -std=$STD -I./include"
TST_CFLAGS="$CFLAGS -DTSTDBG -I/usr/include/sigmatest"
LDFLAGS="-shared"
TST_LDFLAGS="-lstest -L/usr/lib"

SRC_DIR=src
BUILD_DIR=build
BIN_DIR=bin
LIB_DIR="$BIN_DIR/lib"
TEST_DIR=test
TST_BUILD_DIR="$BUILD_DIR/test"

# Package definitions: associative array mapping package names to "output_name | source_list"
declare -A PACKAGES=(
    ["collection"]="sigma.collections | array_base collections list parray farray slotarray"
)

# Required packages (automatically linked)
REQUIRES=("sigma.collections")

# Memory checking (customize as needed)
VALGRIND_ENABLED=true
VALGRIND_OPTS="--leak-check=full --show-leak-kinds=all --track-origins=yes --verbose"
ASAN_ENABLED=false
ASAN_OPTIONS="detect_leaks=1:detect_stack_use_after_return=1:detect_invalid_pointer_pairs=1"

# Build targets (customize as needed)
declare -A BUILD_TARGETS=(
    ["all"]="build_lib"
    ["compile"]="compile_only"
    ["clean"]="clean"
    ["clean_all"]="clean_all"
    ["install"]="build_lib && install_lib"
    ["test"]="run_all_tests"
)

# Test configurations (customize as needed)
declare -A TEST_CONFIGS=(
    ["memory"]="with_proto_objects"
    ["other"]="standard"
)

# Test-specific compile flags (customize as needed)
declare -A TEST_COMPILE_FLAGS=(
    ["bootstrap"]="TEST_BOOTSTRAP_ONLY"
)
EOF

# Make scripts executable
chmod +x cbuild ctest cpack cpub
```

## Extending

Add new targets by editing the `case` statement in `cbuild`. For example:

```bash
debug)
    build_lib  # Custom debug build
    ;;
```

Modify `config.sh` for project-specific settings.