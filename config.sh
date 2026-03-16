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
BASE_CFLAGS="-Wall -Wextra -g -fPIC -std=$STD -I./include -I/usr/local/include"

if [ "$ASAN_ENABLED" = true ]; then
    BASE_CFLAGS="$BASE_CFLAGS -fsanitize=address -fsanitize=undefined"
fi

CFLAGS="$BASE_CFLAGS"
TST_CFLAGS="$CFLAGS -DTSTDBG"
LDFLAGS="-shared"
TST_LDFLAGS="/usr/local/packages/sigma.test.o /usr/local/packages/sigma.text.o -Wl,--wrap=malloc,--wrap=free"

SRC_DIR=src
BUILD_DIR=build
BIN_DIR=bin
LIB_DIR="$BIN_DIR/lib"
TEST_DIR=test
TST_BUILD_DIR="$BUILD_DIR/test"
LIB_NAME="sigma.memory"

# sigma.core provides sc_allocator_i + slab type definitions
# sigma.collections for skip list / b-tree support
REQUIRES=("sigma.core" "sigma.collections")

declare -A PACKAGES=(
    ["memory"]="sigma.memory | memory"
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

