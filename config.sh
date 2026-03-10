# Configuration file for build.sh
# Defines build variables sourced by the main build script

CC=gcc
STD=c2x

# Memory checking configuration (declared early for conditional compilation)
# VALGRIND_ENABLED: Set to true if valgrind is installed on the system
VALGRIND_ENABLED=true
# VALGRIND_OPTS: Default valgrind options
VALGRIND_OPTS="--leak-check=full --show-leak-kinds=all --track-origins=yes --verbose"

# ASAN_ENABLED: Set to true to compile with AddressSanitizer
ASAN_ENABLED=false
# ASAN_OPTIONS: Runtime options for AddressSanitizer
ASAN_OPTIONS="detect_leaks=1:detect_stack_use_after_return=1:detect_invalid_pointer_pairs=1"

# Base compiler flags
BASE_CFLAGS="-Wall -Wextra -g -fPIC -std=$STD -I./include -I/usr/local/include"

# Add ASAN flags if enabled
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

# Requires Sigma.Collections
REQUIRES=("sigma.collections")

# Bundle definitions: associative array mapping bundle names to "output_name | source_list"
declare -A PACKAGES=(
    ["memory"]="sigma.memory | memory node_pool slab_manager"
)

# Build target definitions: associative array mapping targets to commands
# See BUILDING.md for option details
declare -A BUILD_TARGETS=(
    ["all"]="compile_only"
    ["lib"]="build_lib"
    ["clean"]="clean"
    ["clean_all"]="clean_all"
    ["install"]="build_lib && install_lib"
    ["test"]="run_all_tests"
    ["root"]="show_project_info"
)

# Special test configurations: associative array mapping test names to linking strategies
# Requires SigmaTest for test sets
# See BUILDING.md for option details
declare -A TEST_CONFIGS=()

# Special test flags: space-separated list of compiler flags (without -D prefix)
# Flags will be prefixed with -D automatically
# See BUILDING.md for option details
declare -A TEST_COMPILE_FLAGS=(
)

# License copyright header for generated files
LICENSE_COPYRIGHT="/*
 * SigmaCore
 * Copyright (c) \$(YEAR) David Boarman (BadKraft) and contributors
 * QuantumOverride [Q|]
 * ----------------------------------------------
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the \"Software\"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 * ----------------------------------------------
 * File: \$(FILE)
 * Description: <DESCRIPTION_PLACEHOLDER>
 */"

