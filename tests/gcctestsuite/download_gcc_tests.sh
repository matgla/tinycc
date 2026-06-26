#!/bin/bash
# Fetch the GCC c-torture tests used by the TinyCC test suites.
#
# We only ever use gcc/testsuite/gcc.c-torture (~16 MB), but the submodule
# points at the whole gcc-mirror/gcc repo (~1.3 GB working tree, ~4 GB history).
# A plain `git submodule update --init` therefore costs minutes and gigabytes.
#
# Instead we do a *partial* (--filter=blob:none) + *sparse* (only the torture
# directory) checkout of the exact pinned submodule commit. That fetches just
# the torture blobs plus the tree objects needed to reach them: ~16 MB in a few
# seconds, while leaving `git submodule status` clean (HEAD == pinned commit).
#
# Idempotent: if the torture tests are already present (a full submodule
# checkout, or a previous sparse fetch) this is a no-op.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SUPER_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"          # the tinycc repo (owns the submodule)
SUBMODULE_REL="tests/gcctestsuite/gcc-testsuite"
SUBMODULE_PATH="$SUPER_DIR/$SUBMODULE_REL"
SPARSE_DIR="gcc/testsuite/gcc.c-torture"
TORTURE_DIR="$SUBMODULE_PATH/$SPARSE_DIR"

echo "=========================================="
echo "GCC Torture Tests Setup (sparse + partial)"
echo "=========================================="

count_tests() {
    echo "  compile: $(ls "$TORTURE_DIR"/compile/*.c 2>/dev/null | wc -l)  execute: $(ls "$TORTURE_DIR"/execute/*.c 2>/dev/null | wc -l)"
}

# Already present (full or sparse checkout)? Nothing to do.
if [ -d "$TORTURE_DIR/compile" ] && [ -d "$TORTURE_DIR/execute" ]; then
    echo "GCC torture tests already available:"
    echo "  $TORTURE_DIR"
    count_tests
    exit 0
fi

# Resolve the submodule URL and the exact pinned commit from the superproject.
URL="$(git -C "$SUPER_DIR" config -f .gitmodules "submodule.$SUBMODULE_REL.url" 2>/dev/null \
       || echo "https://github.com/gcc-mirror/gcc.git")"
PIN="$(git -C "$SUPER_DIR" rev-parse "HEAD:$SUBMODULE_REL" 2>/dev/null || true)"

echo "URL:           $URL"
echo "Pinned commit: ${PIN:-<unknown — will use default-branch tip>}"
echo "Sparse path:   $SPARSE_DIR"
echo ""

# Partial + sparse fetch of a single committish into the submodule path.
# $1: committish to fetch (a SHA, or empty to use the remote's default HEAD).
sparse_fetch() {
    local committish="$1"
    mkdir -p "$SUBMODULE_PATH"
    if [ ! -e "$SUBMODULE_PATH/.git" ]; then
        git -C "$SUBMODULE_PATH" init -q || return 1
        git -C "$SUBMODULE_PATH" remote add origin "$URL" 2>/dev/null \
            || git -C "$SUBMODULE_PATH" remote set-url origin "$URL" || return 1
    fi
    # Mark as a partial clone and check out only the torture directory, so the
    # blob:none fetch only ever materializes those ~16 MB of files.
    git -C "$SUBMODULE_PATH" config extensions.partialClone origin
    git -C "$SUBMODULE_PATH" config core.sparseCheckout true
    git -C "$SUBMODULE_PATH" sparse-checkout set --no-cone "/$SPARSE_DIR/*" || return 1
    git -C "$SUBMODULE_PATH" fetch --depth 1 --filter=blob:none origin "${committish:-HEAD}" || return 1
    git -C "$SUBMODULE_PATH" checkout -q FETCH_HEAD || return 1
}

echo "Fetching torture tests (sparse + partial)..."
if sparse_fetch "$PIN"; then
    :
elif [ -n "$PIN" ] && sparse_fetch ""; then
    echo "note: pinned commit unavailable; fetched default-branch tip instead" >&2
else
    echo "sparse fetch failed; falling back to a full submodule update" >&2
    rm -rf "${SUBMODULE_PATH:?}/.git"
    git -C "$SUPER_DIR" submodule update --init --depth 1 "$SUBMODULE_REL"
fi

# Normalize the gitdir into the superproject's .git/modules layout so that
# `git submodule status` and future submodule commands treat it like any other
# submodule (best-effort; a standalone .git also works fine for the tests).
git -C "$SUPER_DIR" submodule absorbgitdirs "$SUBMODULE_REL" 2>/dev/null || true

echo ""
echo "Download complete:"
echo "  $TORTURE_DIR"
count_tests
