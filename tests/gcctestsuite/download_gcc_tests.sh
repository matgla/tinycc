#!/bin/bash
# Download or initialize GCC torture tests

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SUBMODULE_PATH="$SCRIPT_DIR/gcc-testsuite"

echo "=========================================="
echo "GCC Torture Tests Setup"
echo "=========================================="
echo ""

# Check if submodule exists and is populated
if [ -d "$SUBMODULE_PATH/gcc/testsuite/gcc.c-torture" ]; then
    echo "GCC torture tests already available via git submodule:"
    echo "  $SUBMODULE_PATH/gcc/testsuite/gcc.c-torture"
    echo ""
    echo "Test counts:"
    echo "  Compile tests: $(ls $SUBMODULE_PATH/gcc/testsuite/gcc.c-torture/compile/*.c 2>/dev/null | wc -l)"
    echo "  Execute tests: $(ls $SUBMODULE_PATH/gcc/testsuite/gcc.c-torture/execute/*.c 2>/dev/null | wc -l)"
    exit 0
fi

# Try to initialize the submodule
echo "Attempting to initialize git submodule..."
cd "$SCRIPT_DIR/../.."
if git submodule update --init --depth 1 tests/gcctestsuite/gcc-testsuite 2>/dev/null; then
    echo ""
    echo "Submodule initialized successfully!"
    echo ""
    echo "Test counts:"
    echo "  Compile tests: $(ls $SUBMODULE_PATH/gcc/testsuite/gcc.c-torture/compile/*.c 2>/dev/null | wc -l)"
    echo "  Execute tests: $(ls $SUBMODULE_PATH/gcc/testsuite/gcc.c-torture/execute/*.c 2>/dev/null | wc -l)"
    exit 0
fi

# Fallback: download to /tmp
echo "Submodule not available. Downloading to /tmp as fallback..."
echo ""

GCC_TESTSUITE_PATH="${GCC_TORTURE_PATH:-/tmp/gcc-testsuite}"

if [ -d "$GCC_TESTSUITE_PATH/gcc/testsuite/gcc.c-torture" ]; then
    echo "GCC torture tests already exist at:"
    echo "  $GCC_TESTSUITE_PATH/gcc/testsuite/gcc.c-torture"
    echo ""
    read -p "Re-download? (y/N): " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        echo "Using existing tests."
        echo ""
        echo "To use these tests, set:"
        echo "  export GCC_TORTURE_PATH=$GCC_TESTSUITE_PATH/gcc/testsuite/gcc.c-torture"
        exit 0
    fi
    rm -rf "$GCC_TESTSUITE_PATH"
fi

echo "Downloading GCC testsuite to:"
echo "  $GCC_TESTSUITE_PATH"
echo ""

mkdir -p "$GCC_TESTSUITE_PATH"
cd "$GCC_TESTSUITE_PATH"

echo "Cloning GCC repository (this may take a few minutes)..."
git clone --depth 1 --filter=blob:none --sparse \
    https://github.com/gcc-mirror/gcc.git \
    "$GCC_TESTSUITE_PATH" 2>&1 | tail -5

echo ""
echo "Checking out testsuite files..."
git sparse-checkout init --cone
git sparse-checkout add gcc/testsuite/gcc.c-torture

echo ""
echo "=========================================="
echo "Download complete!"
echo "=========================================="
echo ""
echo "Test counts:"
echo "  Compile tests: $(ls $GCC_TESTSUITE_PATH/gcc/testsuite/gcc.c-torture/compile/*.c 2>/dev/null | wc -l)"
echo "  Execute tests: $(ls $GCC_TESTSUITE_PATH/gcc/testsuite/gcc.c-torture/execute/*.c 2>/dev/null | wc -l)"
echo ""
echo "To use these tests, set:"
echo "  export GCC_TORTURE_PATH=$GCC_TESTSUITE_PATH/gcc/testsuite/gcc.c-torture"
echo ""
