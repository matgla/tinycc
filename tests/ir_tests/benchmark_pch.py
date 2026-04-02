#!/usr/bin/env python3
"""
Benchmark PCH compile-time impact for a few high-value header scenarios.

Scenarios:
  - libc/common headers: stdio.h + stdlib.h + string.h
  - libtcc.h

Each scenario measures:
  - baseline compile time (no PCH)
  - explicit PCH compile time (-generate-pch + -use-pch)

Implementation note:
  TinyCC's current CLI accepts these snapshots most reliably when the PCH is
  generated from the representative translation unit itself. The benchmark
  sources are intentionally dominated by the target headers, so this still
  captures the header-processing impact these scenarios care about.

Outputs:
  - summary.json
  - summary.csv
  - raw_runs.json

Example:
  python tests/ir_tests/benchmark_pch.py --iterations 5
"""

from __future__ import annotations

import argparse
import csv
import json
import shutil
import statistics
import subprocess
import sys
import textwrap
import time
from pathlib import Path

CURRENT_DIR = Path(__file__).resolve().parent
REPO_ROOT = CURRENT_DIR.parent.parent
DEFAULT_COMPILER = REPO_ROOT / "armv8m-tcc"
DEFAULT_OUTPUT_DIR = CURRENT_DIR / "pch_benchmark_results"
TIME_BINARY = Path("/usr/bin/time")


SCENARIOS = (
    {
        "id": "libc-common",
        "title": "libc/common headers",
        "source_name": "libc_common_bench.c",
        "source_text": """
            #include <stdio.h>
            #include <stdlib.h>
            #include <string.h>

            static int bench_libc(void)
            {
              const char *input = "tinycc-pch";
              size_t len = strlen(input);
              char *copy = malloc(len + 1);
              FILE *stream = stderr;

              if (!copy || !stream) {
                free(copy);
                return EXIT_FAILURE;
              }

              memcpy(copy, input, len + 1);
              fprintf(stream, "%s %zu\\n", copy, len);
              free(copy);
              return EXIT_SUCCESS;
            }

            int main(void)
            {
              return bench_libc();
            }
        """,
        "include_dirs": [],
    },
    {
        "id": "libtcc",
        "title": "libtcc.h",
        "source_name": "libtcc_bench.c",
        "source_text": """
            #include "libtcc.h"

            static void configure_state(TCCState *s)
            {
              tcc_set_error_func(s, 0, 0);
              tcc_add_include_path(s, ".");
              tcc_define_symbol(s, "PCH_BENCH", "1");
              tcc_undefine_symbol(s, "PCH_BENCH");
            }

            int main(void)
            {
              TCCState *s = tcc_new();
              if (!s) {
                return 1;
              }

              configure_state(s);
              tcc_delete(s);
              return 0;
            }
        """,
        "include_dirs": [REPO_ROOT],
    },
)


def _dedent(text: str) -> str:
    return textwrap.dedent(text).lstrip()


def _write_text(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(_dedent(content))


def _compiler_cmd_base(compiler: Path) -> list[str]:
    return [str(compiler), f"-B{REPO_ROOT}"]


def _include_flags(include_dirs: list[Path]) -> list[str]:
    flags: list[str] = []
    for include_dir in include_dirs:
        flags.extend(["-I", str(include_dir)])
    return flags


def _parse_time_metrics(time_file: Path) -> dict[str, float | int]:
    metrics: dict[str, float | int] = {
        "real_time_s": 0.0,
        "user_time_s": 0.0,
        "sys_time_s": 0.0,
        "max_rss_kb": 0,
    }

    if not time_file.exists():
        return metrics

    for line in time_file.read_text().splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        if key == "max_rss_kb":
            try:
                metrics[key] = int(value)
            except ValueError:
                pass
        else:
            try:
                metrics[key] = float(value)
            except ValueError:
                pass

    return metrics


def _run_command(command: list[str], *, time_output: Path | None = None) -> tuple[subprocess.CompletedProcess[str], dict[str, float | int], float]:
    wrapped_command = command
    if time_output is not None and TIME_BINARY.exists():
        if time_output.exists():
            time_output.unlink()
        wrapped_command = [
            str(TIME_BINARY),
            "-f",
            "real_time_s=%e\nuser_time_s=%U\nsys_time_s=%S\nmax_rss_kb=%M",
            "-o",
            str(time_output),
            *command,
        ]

    start = time.perf_counter()
    result = subprocess.run(
        wrapped_command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    elapsed = time.perf_counter() - start
    metrics = _parse_time_metrics(time_output) if time_output is not None else {}
    return result, metrics, elapsed


def _mean(values: list[float]) -> float:
    return statistics.fmean(values) if values else 0.0


def _stdev(values: list[float]) -> float:
    return statistics.stdev(values) if len(values) > 1 else 0.0


def _summarize_runs(runs: list[dict], *, pch_generation_time_s: float | None = None) -> dict:
    compile_times = [run["compile_time_s"] for run in runs]
    real_times = [run["real_time_s"] for run in runs]
    user_times = [run["user_time_s"] for run in runs]
    sys_times = [run["sys_time_s"] for run in runs]
    max_rss_values = [run["max_rss_kb"] for run in runs]

    summary = {
        "runs": len(runs),
        "success": all(run["success"] for run in runs),
        "mean_compile_time_s": _mean(compile_times),
        "median_compile_time_s": statistics.median(compile_times) if compile_times else 0.0,
        "min_compile_time_s": min(compile_times) if compile_times else 0.0,
        "max_compile_time_s": max(compile_times) if compile_times else 0.0,
        "stdev_compile_time_s": _stdev(compile_times),
        "mean_real_time_s": _mean(real_times),
        "mean_user_time_s": _mean(user_times),
        "mean_sys_time_s": _mean(sys_times),
        "max_rss_kb": max(max_rss_values) if max_rss_values else 0,
    }
    if pch_generation_time_s is not None:
        summary["pch_generation_time_s"] = pch_generation_time_s
    return summary


def _speedup(baseline_time: float, current_time: float) -> float:
    if baseline_time <= 0 or current_time <= 0:
        return 0.0
    return baseline_time / current_time


def _relative_change_pct(baseline_time: float, current_time: float) -> float:
    if baseline_time <= 0:
        return 0.0
    return ((current_time - baseline_time) / baseline_time) * 100.0


def _print_summary(summary: dict) -> None:
    print("=" * 80)
    print("PCH BENCHMARK SUMMARY")
    print("=" * 80)
    for scenario in summary["scenarios"]:
        baseline = scenario["modes"]["baseline"]
        explicit_pch = scenario["modes"]["explicit_pch"]
        print(f"{scenario['title']}:")
        print(
            "  baseline    "
            f"mean={baseline['mean_compile_time_s']:.4f}s "
            f"median={baseline['median_compile_time_s']:.4f}s "
            f"rss={baseline['max_rss_kb']}KB"
        )
        print(
            "  explicit PCH "
            f"mean={explicit_pch['mean_compile_time_s']:.4f}s "
            f"median={explicit_pch['median_compile_time_s']:.4f}s "
            f"rss={explicit_pch['max_rss_kb']}KB"
        )
        print(
            "  speedup     "
            f"{scenario['speedup_vs_baseline']:.2f}x "
            f"({scenario['relative_change_pct']:.1f}% vs baseline)"
        )
        print(
            "  pch build    "
            f"{scenario['pch_generation_time_s']:.4f}s"
        )
        print()

    print(f"JSON summary: {summary['summary_json']}")
    print(f"CSV summary:  {summary['summary_csv']}")
    print(f"Raw runs:     {summary['raw_runs_json']}")


def _print_failure(label: str, result: subprocess.CompletedProcess[str]) -> None:
    print(f"{label} failed with exit code {result.returncode}", file=sys.stderr)
    if result.stdout:
        print(result.stdout, file=sys.stderr, end="" if result.stdout.endswith("\n") else "\n")
    if result.stderr:
        print(result.stderr, file=sys.stderr, end="" if result.stderr.endswith("\n") else "\n")


def _benchmark_mode(
    *,
    compiler: Path,
    scenario: dict,
    generated_dir: Path,
    artifacts_dir: Path,
    mode: str,
    iterations: int,
    warmup: int,
    pch_file: Path | None = None,
) -> list[dict]:
    include_dirs = [generated_dir, *scenario["include_dirs"]]
    compile_args = [
        *_compiler_cmd_base(compiler),
        *_include_flags(include_dirs),
        "-c",
        str(generated_dir / scenario["source_name"]),
    ]
    if pch_file is not None:
        compile_args.extend(["-use-pch", str(pch_file)])

    run_count = warmup + iterations
    measured_runs: list[dict] = []
    for idx in range(run_count):
        object_file = artifacts_dir / f"{scenario['id']}_{mode}_{idx}.o"
        time_file = artifacts_dir / f"{scenario['id']}_{mode}_{idx}.time"
        command = [*compile_args, "-o", str(object_file)]
        result, metrics, elapsed = _run_command(command, time_output=time_file)
        combined_output = (result.stderr or "") + (result.stdout or "")
        success = result.returncode == 0 and "ignoring PCH" not in combined_output

        run = {
            "scenario": scenario["id"],
            "mode": mode,
            "iteration": idx + 1,
            "measured": idx >= warmup,
            "success": success,
            "compile_time_s": elapsed,
            "real_time_s": float(metrics.get("real_time_s", 0.0)),
            "user_time_s": float(metrics.get("user_time_s", 0.0)),
            "sys_time_s": float(metrics.get("sys_time_s", 0.0)),
            "max_rss_kb": int(metrics.get("max_rss_kb", 0)),
            "command": command,
            "stdout": result.stdout,
            "stderr": result.stderr,
            "object_file": str(object_file),
        }

        if idx >= warmup:
            measured_runs.append(run)

        if not success:
            _print_failure(f"{scenario['title']} [{mode}]", result)
            if "ignoring PCH" in combined_output:
                print("PCH was ignored; refusing to report misleading benchmark results.", file=sys.stderr)
            raise RuntimeError(f"benchmark run failed for {scenario['id']} [{mode}]")

    return measured_runs


def _generate_pch(*, compiler: Path, scenario: dict, generated_dir: Path, artifacts_dir: Path) -> tuple[Path, float]:
    source_file = generated_dir / scenario["source_name"]
    pch_file = artifacts_dir / f"{scenario['id']}.pch"
    time_file = artifacts_dir / f"{scenario['id']}_generate_pch.time"
    command = [
        *_compiler_cmd_base(compiler),
        *_include_flags([generated_dir, *scenario["include_dirs"]]),
        "-generate-pch",
        str(source_file),
        "-o",
        str(pch_file),
    ]
    result, _metrics, elapsed = _run_command(command, time_output=time_file)
    if result.returncode != 0 or not pch_file.exists():
        _print_failure(f"{scenario['title']} [generate-pch]", result)
        raise RuntimeError(f"failed to generate PCH for {scenario['id']}")
    return pch_file, elapsed


def _write_outputs(output_dir: Path, summary: dict, raw_runs: list[dict]) -> tuple[Path, Path, Path]:
    summary_json = output_dir / "summary.json"
    summary_csv = output_dir / "summary.csv"
    raw_runs_json = output_dir / "raw_runs.json"

    summary_json.write_text(json.dumps(summary, indent=2))
    raw_runs_json.write_text(json.dumps(raw_runs, indent=2))

    with summary_csv.open("w", newline="") as csv_file:
        fieldnames = [
            "scenario",
            "title",
            "mode",
            "runs",
            "success",
            "mean_compile_time_s",
            "median_compile_time_s",
            "min_compile_time_s",
            "max_compile_time_s",
            "stdev_compile_time_s",
            "mean_real_time_s",
            "mean_user_time_s",
            "mean_sys_time_s",
            "max_rss_kb",
            "speedup_vs_baseline",
            "relative_change_pct",
            "pch_generation_time_s",
        ]
        writer = csv.DictWriter(csv_file, fieldnames=fieldnames)
        writer.writeheader()
        for scenario in summary["scenarios"]:
            for mode_name, mode_summary in scenario["modes"].items():
                writer.writerow(
                    {
                        "scenario": scenario["id"],
                        "title": scenario["title"],
                        "mode": mode_name,
                        "runs": mode_summary["runs"],
                        "success": mode_summary["success"],
                        "mean_compile_time_s": f"{mode_summary['mean_compile_time_s']:.9f}",
                        "median_compile_time_s": f"{mode_summary['median_compile_time_s']:.9f}",
                        "min_compile_time_s": f"{mode_summary['min_compile_time_s']:.9f}",
                        "max_compile_time_s": f"{mode_summary['max_compile_time_s']:.9f}",
                        "stdev_compile_time_s": f"{mode_summary['stdev_compile_time_s']:.9f}",
                        "mean_real_time_s": f"{mode_summary['mean_real_time_s']:.9f}",
                        "mean_user_time_s": f"{mode_summary['mean_user_time_s']:.9f}",
                        "mean_sys_time_s": f"{mode_summary['mean_sys_time_s']:.9f}",
                        "max_rss_kb": mode_summary["max_rss_kb"],
                        "speedup_vs_baseline": f"{scenario['speedup_vs_baseline']:.6f}" if mode_name == "explicit_pch" else "",
                        "relative_change_pct": f"{scenario['relative_change_pct']:.6f}" if mode_name == "explicit_pch" else "",
                        "pch_generation_time_s": f"{scenario['pch_generation_time_s']:.9f}" if mode_name == "explicit_pch" else "",
                    }
                )

    return summary_json, summary_csv, raw_runs_json


def _prepare_sources(output_dir: Path) -> tuple[Path, Path]:
    generated_dir = output_dir / "generated"
    artifacts_dir = output_dir / "artifacts"
    generated_dir.mkdir(parents=True, exist_ok=True)
    artifacts_dir.mkdir(parents=True, exist_ok=True)

    for scenario in SCENARIOS:
        _write_text(generated_dir / scenario["source_name"], scenario["source_text"])

    return generated_dir, artifacts_dir


def _compiler_metadata(compiler: Path) -> dict:
    version = subprocess.run(
        [str(compiler), "-v"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    search_dirs = subprocess.run(
        [str(compiler), f"-B{REPO_ROOT}", "-print-search-dirs"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    return {
        "compiler": str(compiler),
        "compiler_version_stdout": version.stdout,
        "compiler_version_stderr": version.stderr,
        "search_dirs_stdout": search_dirs.stdout,
        "search_dirs_stderr": search_dirs.stderr,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Benchmark TinyCC PCH compile-time impact")
    parser.add_argument("--compiler", type=Path, default=DEFAULT_COMPILER, help="Path to armv8m-tcc")
    parser.add_argument("--output-dir", "-o", type=Path, default=DEFAULT_OUTPUT_DIR, help="Directory for generated artifacts and summaries")
    parser.add_argument("--iterations", "-n", type=int, default=5, help="Measured iterations per mode")
    parser.add_argument("--warmup", type=int, default=1, help="Warmup iterations per mode")
    parser.add_argument(
        "--scenario",
        action="append",
        choices=[scenario["id"] for scenario in SCENARIOS],
        help="Restrict benchmark to a specific scenario (repeatable)",
    )
    parser.add_argument("--keep-artifacts", action="store_true", help="Keep prior output directory contents")
    args = parser.parse_args()

    compiler = args.compiler.resolve()
    output_dir = args.output_dir.resolve()

    if not compiler.exists():
        print(f"Compiler not found: {compiler}", file=sys.stderr)
        return 1

    if args.iterations < 1:
        print("--iterations must be >= 1", file=sys.stderr)
        return 1

    if args.warmup < 0:
        print("--warmup must be >= 0", file=sys.stderr)
        return 1

    if output_dir.exists() and not args.keep_artifacts:
        shutil.rmtree(output_dir)

    selected_ids = set(args.scenario or [scenario["id"] for scenario in SCENARIOS])
    scenarios = [scenario for scenario in SCENARIOS if scenario["id"] in selected_ids]

    generated_dir, artifacts_dir = _prepare_sources(output_dir)
    raw_runs: list[dict] = []
    summary = {
        "compiler": _compiler_metadata(compiler),
        "output_dir": str(output_dir),
        "iterations": args.iterations,
        "warmup": args.warmup,
        "scenarios": [],
    }

    for scenario in scenarios:
        pch_file, pch_generation_time_s = _generate_pch(
            compiler=compiler,
            scenario=scenario,
            generated_dir=generated_dir,
            artifacts_dir=artifacts_dir,
        )
        baseline_runs = _benchmark_mode(
            compiler=compiler,
            scenario=scenario,
            generated_dir=generated_dir,
            artifacts_dir=artifacts_dir,
            mode="baseline",
            iterations=args.iterations,
            warmup=args.warmup,
        )
        explicit_pch_runs = _benchmark_mode(
            compiler=compiler,
            scenario=scenario,
            generated_dir=generated_dir,
            artifacts_dir=artifacts_dir,
            mode="explicit_pch",
            iterations=args.iterations,
            warmup=args.warmup,
            pch_file=pch_file,
        )
        raw_runs.extend(baseline_runs)
        raw_runs.extend(explicit_pch_runs)

        baseline_summary = _summarize_runs(baseline_runs)
        explicit_pch_summary = _summarize_runs(explicit_pch_runs, pch_generation_time_s=pch_generation_time_s)
        baseline_time = baseline_summary["mean_compile_time_s"]
        explicit_time = explicit_pch_summary["mean_compile_time_s"]

        scenario_summary = {
            "id": scenario["id"],
            "title": scenario["title"],
            "pch_file": str(pch_file),
            "pch_generation_time_s": pch_generation_time_s,
            "speedup_vs_baseline": _speedup(baseline_time, explicit_time),
            "relative_change_pct": _relative_change_pct(baseline_time, explicit_time),
            "modes": {
                "baseline": baseline_summary,
                "explicit_pch": explicit_pch_summary,
            },
        }
        summary["scenarios"].append(scenario_summary)

    summary_json, summary_csv, raw_runs_json = _write_outputs(output_dir, summary, raw_runs)
    summary["summary_json"] = str(summary_json)
    summary["summary_csv"] = str(summary_csv)
    summary["raw_runs_json"] = str(raw_runs_json)
    summary_json.write_text(json.dumps(summary, indent=2))

    _print_summary(summary)
    return 0


if __name__ == "__main__":
    sys.exit(main())
