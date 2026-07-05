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

# A handful of torture tests #include a file from a *sibling* testsuite
# directory, e.g. execute/pr30314.c does
#     #include "../../gcc.dg/tree-ssa/pr30314.c"
# Those files live OUTSIDE the gcc.c-torture sparse path, so a sparse checkout
# omits them and the compile fails with "include file '...' not found" (the test
# harness uploads such includes to the device too, but only if they exist on
# disk). Scan the checked-out tests for "../"-escaping quoted includes, resolve
# each to a path inside this submodule, and sparse-add exactly those files. Loop
# a few times so an included file that itself pulls in another out-of-tree file
# is covered as well.
#
# No-op on a full checkout (every file is already present); guarded on the
# sparse-checkout config so we never slow-scan a full gcc working tree.
fetch_extra_includes() {
    [ "$(git -C "$SUBMODULE_PATH" config --get core.sparseCheckout 2>/dev/null)" = "true" ] || return 0
    local scan_dir="$SUBMODULE_PATH/gcc/testsuite"
    [ -d "$scan_dir" ] || return 0

    local pass
    for pass in 1 2 3; do
        local -a missing=()
        local line file inc abs rel
        # grep -H prints "FILE:#include "...""; split on the ":#" before the
        # directive to recover the including file, then pull the quoted path.
        while IFS= read -r line; do
            file="${line%%:#*}"
            inc="$(printf '%s\n' "$line" | sed -E 's/.*"([^"]+)".*/\1/')"
            [ -n "$file" ] && [ -n "$inc" ] || continue
            abs="$(realpath -m "$(dirname "$file")/$inc" 2>/dev/null)" || continue
            case "$abs" in
                "$SUBMODULE_PATH"/*) rel="${abs#"$SUBMODULE_PATH/"}" ;;
                *) continue ;;  # include escapes the submodule entirely; skip
            esac
            [ -f "$SUBMODULE_PATH/$rel" ] || missing+=("$rel")
        done < <(grep -rHoE --include='*.c' \
                     '#[[:space:]]*include[[:space:]]*"\.\.[^"]*"' "$scan_dir" 2>/dev/null)

        [ "${#missing[@]}" -eq 0 ] && return 0

        local -a uniq=()
        local m
        while IFS= read -r m; do
            [ -n "$m" ] && uniq+=("/$m")  # leading "/" anchors the no-cone pattern at repo root
        done < <(printf '%s\n' "${missing[@]}" | sort -u)

        echo "  fetching ${#uniq[@]} out-of-tree include file(s) referenced by torture tests"
        # The repo is already in no-cone mode (set during sparse_fetch), so `add`
        # inherits it; a partial clone lazily fetches the newly in-scope blobs.
        git -C "$SUBMODULE_PATH" sparse-checkout add "${uniq[@]}" || {
            echo "warning: could not sparse-add include files: ${uniq[*]}" >&2
            return 0
        }
    done
}

# Already present (full or sparse checkout)? Nothing to fetch — but still make
# sure the out-of-tree include files are there (a sparse checkout from before
# this script learned to fetch them would be missing them).
if [ -d "$TORTURE_DIR/compile" ] && [ -d "$TORTURE_DIR/execute" ]; then
    echo "GCC torture tests already available:"
    echo "  $TORTURE_DIR"
    fetch_extra_includes
    count_tests
    exit 0
fi

# Resolve the submodule URL and the exact pinned commit from the superproject.
URL="$(git -C "$SUPER_DIR" config -f .gitmodules "submodule.$SUBMODULE_REL.url" 2>/dev/null \
       || echo "https://github.com/gcc-mirror/gcc.git")"

# Read the pinned submodule commit from the superproject's HEAD tree. This is the
# *only* fatal git operation against the superproject — if it yields nothing the
# script refuses to run (see below) — so it must survive CI's most common gotcha:
# the job container runs as root while the workspace was checked out by a
# different uid, so git's "dubious ownership" guard makes every superproject git
# command fail. That failure is otherwise swallowed by `2>/dev/null`, leaving PIN
# empty and tripping the refuse-to-fetch guard. So on an empty result, register
# the superproject as a safe directory and retry once before giving up.
resolve_pin() { git -C "$SUPER_DIR" rev-parse "HEAD:$SUBMODULE_REL" 2>/dev/null; }
PIN="$(resolve_pin || true)"
if [ -z "$PIN" ]; then
    git config --global --add safe.directory "$SUPER_DIR" 2>/dev/null || true
    PIN="$(resolve_pin || true)"
fi

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

# A non-sparse but still *pinned* fetch into the submodule path: fetch only the
# pinned commit (all blobs, depth 1) and check it out. Used as the fallback when
# the fast partial+sparse fetch doesn't work. It talks to the remote directly
# rather than going through `git submodule update`, so it is unaffected by the
# submodule's `update = none` setting in .gitmodules (which makes the recursive
# checkout — and `submodule update` — skip this submodule entirely).
full_fetch() {
    local committish="$1"
    [ -n "$committish" ] || return 1
    rm -rf "${SUBMODULE_PATH:?}/.git"
    mkdir -p "$SUBMODULE_PATH"
    git -C "$SUBMODULE_PATH" init -q || return 1
    git -C "$SUBMODULE_PATH" remote add origin "$URL" 2>/dev/null \
        || git -C "$SUBMODULE_PATH" remote set-url origin "$URL" || return 1
    git -C "$SUBMODULE_PATH" fetch --depth 1 origin "$committish" || return 1
    git -C "$SUBMODULE_PATH" checkout -q FETCH_HEAD || return 1
}

echo "Fetching torture tests (sparse + partial)..."
if [ -z "$PIN" ]; then
    # IMPORTANT: never fetch the remote's default branch as a fallback. Doing so
    # would silently pull the *current gcc master tip* instead of the pinned
    # commit, so CI would test against an ever-advancing gcc and fail on
    # brand-new upstream tests that didn't exist when the submodule was pinned.
    echo "error: could not resolve the pinned gcc-testsuite commit; refusing to" >&2
    echo "       fetch a moving default branch. Is the submodule gitlink present?" >&2
    exit 1
elif sparse_fetch "$PIN"; then
    :
else
    # The fast partial+sparse fetch of the pinned SHA didn't work (e.g. an old
    # git, or a server that refuses a blob:none fetch of a non-tip SHA). Fall
    # back to a correct, still *pinned* full fetch (slower — it pulls the whole
    # gcc tree at that commit — but it tests exactly the pin).
    echo "sparse fetch of pinned commit $PIN failed; doing a full (pinned) fetch" >&2
    full_fetch "$PIN" || {
        echo "error: could not fetch pinned gcc-testsuite commit $PIN" >&2
        exit 1
    }
fi

# Fetch the few out-of-tree files torture tests #include (gcc.dg/, gcc.target/)
# while the gitdir is still standalone and the just-fetched objects are local.
fetch_extra_includes

# Normalize the gitdir into the superproject's .git/modules layout so that
# `git submodule status` and future submodule commands treat it like any other
# submodule (best-effort; a standalone .git also works fine for the tests).
git -C "$SUPER_DIR" submodule absorbgitdirs "$SUBMODULE_REL" 2>/dev/null || true

echo ""
echo "Download complete:"
echo "  $TORTURE_DIR"
count_tests
