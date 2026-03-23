# Configuration file for build.sh
# Sigma.Memory v0.3.0 — Controller Model rewrite
# Defines build variables sourced by the main build script

CC=gcc
STD=c2x

# Memory checking configuration
VALGRIND_ENABLED=true
VALGRIND_OPTS="--leak-check=full --show-leak-kinds=all --track-origins=yes --verbose"

ASAN_ENABLED=false
ASAN_OPTIONS="detect_leaks=1:detect_stack_use_after_return=1:detect_invalid_pointer_pairs=1"

# Base compiler flags
BASE_CFLAGS="-Wall -Wextra -g -fPIC -std=$STD -I./include -I../sigma.core/package/include -I/usr/local/include"

if [ "$ASAN_ENABLED" = true ]; then
    BASE_CFLAGS="$BASE_CFLAGS -fsanitize=address -fsanitize=undefined"
fi

CFLAGS="$BASE_CFLAGS"
TST_CFLAGS="$CFLAGS -DTSTDBG"
LDFLAGS="-shared"
TST_LDFLAGS="./build/sigma.test.o /usr/local/packages/sigma.core.module.o /usr/local/packages/sigma.core.text.o /usr/local/packages/sigma.core.utils.o /usr/local/packages/sigma.core.o -Wl,--allow-multiple-definition"

SRC_DIR=src
BUILD_DIR=build
BIN_DIR=bin
LIB_DIR="$BIN_DIR/lib"
TEST_DIR=test/unit
TST_BUILD_DIR="$BUILD_DIR/test"
LIB_NAME="sigma.memory"

# sigma.core provides headers only (no .o package needed — memory.c defines Allocator)
# sigma.collections added in Phase 3 for MTIS skip-list / B-tree internals
REQUIRES=()

declare -A PACKAGES=(
    ["memory"]="sigma.memory | memory module"
)

declare -A BUILD_TARGETS=(
    ["all"]="compile_only"
    ["lib"]="build_lib"
    ["clean"]="clean"
    ["clean_all"]="clean_all"
    ["install"]="build_lib && install_lib"
    ["test"]="run_all_tests"
    ["root"]="show_project_info"
)

# Test set configurations — one entry per test/unit/test_<name>.c file.
# Phase 0: RED state — tests exist but memory.c not yet written.
# Phases 1–5 will add entries as each suite goes green.
declare -A TEST_CONFIGS=(
    ["bootstrap"]="standard"  # BST-01..06  Phase 1
    ["slab"]="standard"       # SLB-01..04  Phase 1
    ["bump"]="standard"       # BC-01..12   Phase 2
    ["reclaim"]="standard"    # RC-01..14   Phase 3
    ["kernel"]="standard"     # KNL-01..10  Phase 3B
    ["registry"]="standard"   # REG-01..08  Phase 4
    ["facade"]="standard"     # FAC-01..06  Phase 5
    ["trusted"]="standard"    # TRS-01..07  Phase 6
    ["trusted_app"]="standard" # FTA-01..04  FT-14
    ["alloc_direct"]="standard" # direct alloc tests
)
