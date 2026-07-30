"""
Importable modules backing the runner scripts in ``scripts/``.

Nothing in here is meant to be executed directly -- each file is a library
imported by a runner one level up (``from sources.disasm_common import ...``).
Runners resolve this package because Python puts the runner's own directory
(``scripts/``) on ``sys.path``.
"""
