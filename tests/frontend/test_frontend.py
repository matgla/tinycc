"""Frontend coverage tests for the ARMv8-M TinyCC fork.

Three test modes live under libs/tinycc/tests/frontend/:

* pp/          - preprocessor/lexer golden-output tests
* types/       - type-system / semantic-analysis golden-IR tests
* diagnostics/ - expected-error diagnostic substring tests

Usage:
    pytest tests/frontend/
    pytest tests/frontend/ --update          # regenerate .expect / .stderr files
    pytest tests/frontend/ --compiler /path/to/armv8m-tcc
    pytest tests/frontend/ -k pp             # only preprocessor tests
"""

import difflib
import re
import subprocess
from pathlib import Path

import pytest

FRONTEND_DIR = Path(__file__).parent
TINYCC_DIR = FRONTEND_DIR / "../.."

DEBUG_COMPILER_CANDIDATES = [
    TINYCC_DIR / "bin" / "armv8m-tcc.debug",
    TINYCC_DIR / "armv8m-tcc.debug",
]


def _find_debug_compiler(frontend_compiler):
    """Return a compiler that supports -dump-ir.

    If ``frontend_compiler`` already supports -dump-ir it is returned unchanged.
    Otherwise the nearby armv8m-tcc.debug binary is tried.
    """
    probe = subprocess.run(
        [str(frontend_compiler), "-dump-ir", "-c", "-x", "c", "-", "-o", "/dev/null"],
        input="int f(int x){return x;}",
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    if probe.returncode == 0 and "=== IR" in probe.stdout:
        return frontend_compiler

    for cand in DEBUG_COMPILER_CANDIDATES:
        if cand.exists():
            probe = subprocess.run(
                [str(cand), "-dump-ir", "-c", "-x", "c", "-", "-o", "/dev/null"],
                input="int f(int x){return x;}",
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            if probe.returncode == 0 and "=== IR" in probe.stdout:
                return cand

    raise RuntimeError(
        f"Compiler {frontend_compiler} does not support -dump-ir "
        "and no armv8m-tcc.debug binary was found."
    )


def _strip_builtin_preamble(output):
    """Remove the armv8m-tcc builtin declaration preamble from preprocessor output.

    The cross compiler prepends declarations such as
    ``typedef char*__builtin_va_list;`` and ``void __tcc_va_start(...);``.
    These are not part of the preprocessor construct under test, so drop
    leading lines that look like builtin declarations.
    """
    lines = output.splitlines()
    i = 0
    while i < len(lines):
        line = lines[i].strip()
        if not line:
            i += 1
            continue
        if ("__builtin_" in line or "__tcc_" in line) and "=" not in line and line.rstrip().endswith(";"):
            i += 1
        else:
            break
    return "\n".join(lines[i:]).rstrip() + "\n"


def _normalize_predefined_macros(output):
    """Replace __DATE__ and __TIME__ values with stable placeholders.

    These predefined macros are non-deterministic across runs, so we
    normalize them to keep golden preprocessor output stable.
    """
    # "Mmm dd yyyy" (day is space-padded to width 2, so single-digit days
    # yield two spaces, e.g. "Jul  1 2026")
    output = re.sub(r'"[A-Z][a-z]{2}\s+\d{1,2} \d{4}"', '"<DATE>"', output)
    # "hh:mm:ss"
    output = re.sub(r'"\d{2}:\d{2}:\d{2}"', '"<TIME>"', output)
    return output


_GLOBALSYM_RE = re.compile(r"GlobalSym\((\d+)\)")


def _canonicalize_symbols(text):
    """Renumber `GlobalSym(<tok>)` to `GlobalSym(#<n>)` by first appearance.

    The number the dumper prints is a frontend token id, so it shifts whenever
    the predefined-token set changes -- e.g. dropping the `arm` / `arm_elf`
    machine defines moved every id down by two and broke eight goldens that no
    frontend change had touched.  Pin the symbol's IDENTITY instead: equal ids
    stay equal, different ids stay different, the absolute value stops
    mattering.  Same canonicalization as tests/ir_tests/test_golden_ir.py.
    """
    mapping = {}

    def repl(match):
        tok = match.group(1)
        if tok not in mapping:
            mapping[tok] = len(mapping)
        return f"GlobalSym(#{mapping[tok]})"

    return _GLOBALSYM_RE.sub(repl, text)


def _discover_cases(mode, golden_ext):
    """Return [(case_name, c_file, golden_file), ...] for a frontend mode."""
    mode_dir = FRONTEND_DIR / mode
    cases = []
    if not mode_dir.exists():
        return cases
    for c_file in sorted(mode_dir.glob("*.c")):
        golden = c_file.with_suffix(golden_ext)
        cases.append((c_file.stem, c_file, golden))
    return cases


def _run_compiler(compiler, cflags, c_file, tmp_path, output_object=True):
    """Run the compiler and return the completed process and command line.

    For preprocessor-only mode the object output is omitted so that the
    preprocessed source is emitted on stdout (matching the legacy tests/pp
    behaviour).

    stdout and stderr are captured separately: golden comparisons (pp/ and
    types/ IR dumps) read deterministic stdout only, so debug builds with
    TCC_LOG_* scopes enabled (which log to stderr) don't pollute them, while
    diagnostics tests read stderr.
    """
    cmd = [str(compiler), *cflags, str(c_file)]
    if output_object:
        out_file = tmp_path / f"{c_file.stem}.o"
        cmd.extend(["-o", str(out_file)])
    result = subprocess.run(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    return result, cmd


# ---------------------------------------------------------------------------
# pp/ mode: preprocessor/lexer golden-output tests
# ---------------------------------------------------------------------------
PP_CASES = _discover_cases("pp", ".expect")
PP_CASE_IDS = [name for name, _, _ in PP_CASES]


@pytest.fixture(scope="session")
def debug_compiler(frontend_compiler):
    # The types/ mode needs a compiler built with CONFIG_TCC_DEBUG so that
    # -dump-ir actually emits IR. A plain `make cross` build does not enable
    # it, so skip (rather than error) when no -dump-ir-capable compiler or
    # nearby armv8m-tcc.debug binary is available.
    try:
        return _find_debug_compiler(frontend_compiler)
    except RuntimeError as exc:
        pytest.skip(str(exc))


@pytest.mark.parametrize("name,c_file,golden", PP_CASES, ids=PP_CASE_IDS)
@pytest.mark.frontend
@pytest.mark.frontend_pp
def test_pp(name, c_file, golden, frontend_compiler, tmp_path, request):
    updating = request.config.getoption("--update")
    # Note: -c overrides -E in this tcc fork and suppresses preprocessor output,
    # so we use -E -P to actually exercise the preprocessor/lexer.
    result, cmd = _run_compiler(
        frontend_compiler, ["-E", "-P"], c_file, tmp_path, output_object=False
    )

    if result.returncode != 0:
        raise AssertionError(
            f"Preprocessing failed for pp/{name}\n"
            f"Command: {' '.join(cmd)}\n"
            f"Output:\n{result.stdout}\n"
            f"Stderr:\n{result.stderr}"
        )

    actual = _normalize_predefined_macros(_strip_builtin_preamble(result.stdout))

    if updating:
        golden.write_text(actual)
        return

    if not golden.exists():
        pytest.fail(f"Expected file missing: {golden} (run with --update)")

    expected = golden.read_text()
    if actual != expected:
        diff = "\n".join(
            difflib.unified_diff(
                expected.splitlines(),
                actual.splitlines(),
                fromfile=str(golden),
                tofile=f"<actual pp/{name}>",
                lineterm="",
            )
        )
        raise AssertionError(f"Preprocessor mismatch for pp/{name}\n\n{diff}")


# ---------------------------------------------------------------------------
# types/ mode: type-system / semantic-analysis golden-IR tests
# ---------------------------------------------------------------------------
TYPES_CASES = _discover_cases("types", ".expect")
TYPES_CASE_IDS = [name for name, _, _ in TYPES_CASES]


@pytest.mark.parametrize("name,c_file,golden", TYPES_CASES, ids=TYPES_CASE_IDS)
@pytest.mark.frontend
@pytest.mark.frontend_types
def test_types(name, c_file, golden, debug_compiler, tmp_path, request):
    updating = request.config.getoption("--update")
    # -mfpu=none pins the float lowering the goldens were recorded under.
    # Without it the IR for a float expression depends on how the compiler was
    # configured: a build with CONFIG_TCC_DEFAULT_FPU set to a VFP unit emits
    # FADD where these goldens expect the __aeabi_fadd call. Float codegen per
    # FPU/ABI is what tests/ir_tests and `make test-fp` cover.
    result, cmd = _run_compiler(
        debug_compiler, ["-dump-ir", "-mfpu=none", "-c"], c_file, tmp_path
    )

    if result.returncode != 0:
        raise AssertionError(
            f"Compilation failed for types/{name}\n"
            f"Command: {' '.join(cmd)}\n"
            f"Output:\n{result.stdout}\n"
            f"Stderr:\n{result.stderr}"
        )

    actual = _canonicalize_symbols(result.stdout)

    if updating:
        golden.write_text(actual)
        return

    if not golden.exists():
        pytest.fail(f"Expected file missing: {golden} (run with --update)")

    expected = golden.read_text()
    if actual != expected:
        diff = "\n".join(
            difflib.unified_diff(
                expected.splitlines(),
                actual.splitlines(),
                fromfile=str(golden),
                tofile=f"<actual types/{name}>",
                lineterm="",
            )
        )
        raise AssertionError(f"IR mismatch for types/{name}\n\n{diff}")


# ---------------------------------------------------------------------------
# diagnostics/ mode: expected-error diagnostic substring tests
# ---------------------------------------------------------------------------
DIAGNOSTICS_CASES = _discover_cases("diagnostics", ".stderr")
DIAGNOSTICS_CASE_IDS = [name for name, _, _ in DIAGNOSTICS_CASES]


@pytest.mark.parametrize(
    "name,c_file,golden", DIAGNOSTICS_CASES, ids=DIAGNOSTICS_CASE_IDS
)
@pytest.mark.frontend
@pytest.mark.frontend_diagnostics
def test_diagnostics(
    name, c_file, golden, frontend_compiler, tmp_path, request
):
    updating = request.config.getoption("--update")
    result, cmd = _run_compiler(
        frontend_compiler, ["-Werror", "-c"], c_file, tmp_path
    )

    if result.returncode == 0:
        raise AssertionError(
            f"Expected compilation to fail for diagnostics/{name}\n"
            f"Command: {' '.join(cmd)}"
        )

    actual = result.stderr

    if updating:
        golden.write_text(actual)
        return

    if not golden.exists():
        pytest.fail(f"Stderr file missing: {golden} (run with --update)")

    expected_lines = [
        line for line in golden.read_text().splitlines() if line.strip() != ""
    ]
    missing = [line for line in expected_lines if line not in actual]
    if missing:
        raise AssertionError(
            f"Diagnostic substring(s) missing for diagnostics/{name}\n"
            f"Command: {' '.join(cmd)}\n"
            f"Missing substrings:\n" + "\n".join(f"  - {m!r}" for m in missing)
        )
