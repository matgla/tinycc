#!/bin/bash
# Thin wrapper for compare_disasm.py (preserves backwards compatibility)
exec "$(dirname "$0")/compare_disasm.py" "$@"
