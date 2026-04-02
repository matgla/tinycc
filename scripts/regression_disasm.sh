#!/bin/bash
# Thin wrapper for regression_disasm.py (preserves backwards compatibility)
exec "$(dirname "$0")/regression_disasm.py" "$@"
