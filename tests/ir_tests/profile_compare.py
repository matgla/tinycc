#!/usr/bin/env python3
"""
Compare profiling results between two compiler versions/builds.

Usage:
    # Compare two profile runs
    python profile_compare.py baseline.json current.json

    # Save current run as baseline
    python profile_compare.py --save-baseline profile_results/summary.json --name v1.0

    # Compare against saved baseline
    python profile_compare.py --load-baseline v1.0 profile_results/summary.json

    # Generate HTML report
    python profile_compare.py baseline.json current.json --html report.html

    # Generate Markdown (for gist)
    python profile_compare.py baseline.json current.json --markdown report.md

    # Upload to GitHub Gist (requires gh CLI or GITHUB_TOKEN)
    python profile_compare.py baseline.json current.json --gist

Git Comparison (automated build & profile):
    # Compare a git tag/branch/commit against current HEAD
    python profile_compare.py --git-compare v0.9.27

    # Compare two specific commits
    python profile_compare.py --git-compare abc123 --git-current def456

    # Quick comparison with limited tests
    python profile_compare.py --git-compare v0.9.27 --limit 10

    # Use time profiler instead of heaptrack
    python profile_compare.py --git-compare v0.9.27 --profiler time

    # Generate HTML report from git comparison
    python profile_compare.py --git-compare v0.9.27 --html report.html

Output formats:
    - Console: colored diff table
    - Markdown: gist-friendly table
    - HTML: self-contained page with charts
    - Gist: auto-upload markdown to GitHub
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Optional

from _venv_bootstrap import ensure_venv

ensure_venv()

CURRENT_DIR = Path(__file__).parent
BASELINES_DIR = CURRENT_DIR / "profile_baselines"
REPO_ROOT = CURRENT_DIR.parent.parent  # tinycc root


@dataclass
class Comparison:
    """Comparison between two values."""
    name: str
    baseline: float
    current: float
    unit: str = ""
    higher_is_better: bool = False

    @property
    def diff(self) -> float:
        return self.current - self.baseline

    @property
    def diff_pct(self) -> float:
        if self.baseline == 0:
            return 0 if self.current == 0 else float('inf')
        return (self.diff / self.baseline) * 100

    @property
    def is_better(self) -> bool:
        if self.higher_is_better:
            return self.current >= self.baseline
        return self.current <= self.baseline

    @property
    def is_significant(self) -> bool:
        """Consider >5% change as significant."""
        return abs(self.diff_pct) > 5

    def format_diff(self) -> str:
        sign = "+" if self.diff > 0 else ""
        return f"{sign}{self.diff:.2f}{self.unit} ({sign}{self.diff_pct:.1f}%)"


@dataclass
class TestComparison:
    """Comparison for a single test."""
    test_name: str
    heap_peak: Optional[Comparison] = None
    max_rss: Optional[Comparison] = None
    compile_time: Optional[Comparison] = None
    binary_size: Optional[Comparison] = None
    allocations: Optional[Comparison] = None
    baseline_success: bool = True
    current_success: bool = True


def load_profile(path: Path) -> dict:
    """Load profile JSON file."""
    with open(path) as f:
        data = json.load(f)
    # Convert list to dict keyed by test_name
    if isinstance(data, list):
        return {item["test_name"]: item for item in data}
    return data


def compare_profiles(baseline: dict, current: dict) -> list[TestComparison]:
    """Compare two profile results."""
    comparisons = []

    all_tests = set(baseline.keys()) | set(current.keys())

    for test_name in sorted(all_tests):
        b = baseline.get(test_name, {})
        c = current.get(test_name, {})

        tc = TestComparison(
            test_name=test_name,
            baseline_success=b.get("success", False),
            current_success=c.get("success", False),
        )

        # Only compare if both succeeded
        if tc.baseline_success and tc.current_success:
            if b.get("heap_peak_kb", 0) > 0 or c.get("heap_peak_kb", 0) > 0:
                tc.heap_peak = Comparison(
                    "Heap Peak",
                    b.get("heap_peak_kb", 0),
                    c.get("heap_peak_kb", 0),
                    "KB"
                )

            if b.get("max_rss_kb", 0) > 0 or c.get("max_rss_kb", 0) > 0:
                tc.max_rss = Comparison(
                    "Max RSS",
                    b.get("max_rss_kb", 0),
                    c.get("max_rss_kb", 0),
                    "KB"
                )

            tc.compile_time = Comparison(
                "Compile Time",
                b.get("compile_time_s", 0),
                c.get("compile_time_s", 0),
                "s"
            )

            tc.binary_size = Comparison(
                "Binary Size",
                b.get("total_size", 0),
                c.get("total_size", 0),
                "B"
            )

            if b.get("heap_allocations", 0) > 0 or c.get("heap_allocations", 0) > 0:
                tc.allocations = Comparison(
                    "Allocations",
                    b.get("heap_allocations", 0),
                    c.get("heap_allocations", 0),
                )

        comparisons.append(tc)

    return comparisons


def compute_summary(comparisons: list[TestComparison]) -> dict:
    """Compute summary statistics."""
    successful = [c for c in comparisons if c.baseline_success and c.current_success]

    summary = {
        "total_tests": len(comparisons),
        "both_passed": len(successful),
        "baseline_only": len([c for c in comparisons if c.baseline_success and not c.current_success]),
        "current_only": len([c for c in comparisons if not c.baseline_success and c.current_success]),
        "both_failed": len([c for c in comparisons if not c.baseline_success and not c.current_success]),
    }

    if successful:
        # Aggregate metrics
        if successful[0].heap_peak:
            baseline_heap = sum(c.heap_peak.baseline for c in successful if c.heap_peak)
            current_heap = sum(c.heap_peak.current for c in successful if c.heap_peak)
            summary["total_heap_baseline_kb"] = baseline_heap
            summary["total_heap_current_kb"] = current_heap
            summary["heap_diff_pct"] = ((current_heap - baseline_heap) / baseline_heap * 100) if baseline_heap else 0
            summary["max_heap_baseline_kb"] = max(c.heap_peak.baseline for c in successful if c.heap_peak)
            summary["max_heap_current_kb"] = max(c.heap_peak.current for c in successful if c.heap_peak)

        # Max RSS (only if any test reports RSS)
        if any(c.max_rss and (c.max_rss.baseline > 0 or c.max_rss.current > 0) for c in successful):
            rss_baseline_vals = [c.max_rss.baseline for c in successful if c.max_rss]
            rss_current_vals = [c.max_rss.current for c in successful if c.max_rss]
            if rss_baseline_vals and rss_current_vals:
                summary["max_rss_baseline_kb"] = max(rss_baseline_vals)
                summary["max_rss_current_kb"] = max(rss_current_vals)
                b = summary["max_rss_baseline_kb"]
                c = summary["max_rss_current_kb"]
                summary["rss_diff_pct"] = ((c - b) / b * 100) if b else 0

        baseline_time = sum(c.compile_time.baseline for c in successful if c.compile_time)
        current_time = sum(c.compile_time.current for c in successful if c.compile_time)
        summary["total_time_baseline_s"] = baseline_time
        summary["total_time_current_s"] = current_time
        summary["time_diff_pct"] = ((current_time - baseline_time) / baseline_time * 100) if baseline_time else 0

        baseline_size = sum(c.binary_size.baseline for c in successful if c.binary_size)
        current_size = sum(c.binary_size.current for c in successful if c.binary_size)
        summary["total_size_baseline_b"] = baseline_size
        summary["total_size_current_b"] = current_size
        summary["size_diff_pct"] = ((current_size - baseline_size) / baseline_size * 100) if baseline_size else 0

    return summary


def format_console(comparisons: list[TestComparison], summary: dict, baseline_name: str, current_name: str) -> str:
    """Format comparison for console output."""
    lines = []

    lines.append(f"Comparing: {baseline_name} vs {current_name}")
    lines.append("=" * 80)

    # Summary
    lines.append(f"\nSUMMARY")
    lines.append(f"  Tests: {summary['both_passed']}/{summary['total_tests']} passed in both")

    if "heap_diff_pct" in summary:
        sign = "+" if summary["heap_diff_pct"] > 0 else ""
        lines.append(f"  Max Heap: {summary['max_heap_baseline_kb']}KB -> {summary['max_heap_current_kb']}KB ({sign}{summary['heap_diff_pct']:.1f}%)")

    if "rss_diff_pct" in summary:
        sign = "+" if summary["rss_diff_pct"] > 0 else ""
        lines.append(f"  Max RSS: {summary['max_rss_baseline_kb']}KB -> {summary['max_rss_current_kb']}KB ({sign}{summary['rss_diff_pct']:.1f}%)")

    sign = "+" if summary["time_diff_pct"] > 0 else ""
    lines.append(f"  Total Time: {summary['total_time_baseline_s']:.2f}s -> {summary['total_time_current_s']:.2f}s ({sign}{summary['time_diff_pct']:.1f}%)")

    sign = "+" if summary["size_diff_pct"] > 0 else ""
    lines.append(f"  Total Size: {summary['total_size_baseline_b']}B -> {summary['total_size_current_b']}B ({sign}{summary['size_diff_pct']:.1f}%)")

    # Regressions
    regressions = []
    improvements = []
    for c in comparisons:
        if c.heap_peak and c.heap_peak.is_significant and not c.heap_peak.is_better:
            regressions.append((c.test_name, "heap", c.heap_peak))
        elif c.heap_peak and c.heap_peak.is_significant and c.heap_peak.is_better:
            improvements.append((c.test_name, "heap", c.heap_peak))

        if c.max_rss and c.max_rss.is_significant and not c.max_rss.is_better:
            regressions.append((c.test_name, "rss", c.max_rss))
        elif c.max_rss and c.max_rss.is_significant and c.max_rss.is_better:
            improvements.append((c.test_name, "rss", c.max_rss))

        if c.binary_size and c.binary_size.is_significant and not c.binary_size.is_better:
            regressions.append((c.test_name, "size", c.binary_size))
        elif c.binary_size and c.binary_size.is_significant and c.binary_size.is_better:
            improvements.append((c.test_name, "size", c.binary_size))

    if regressions:
        lines.append(f"\nREGRESSIONS ({len(regressions)}):")
        for test, metric, comp in regressions[:10]:
            lines.append(f"  {test}: {metric} {comp.format_diff()}")

    if improvements:
        lines.append(f"\nIMPROVEMENTS ({len(improvements)}):")
        for test, metric, comp in improvements[:10]:
            lines.append(f"  {test}: {metric} {comp.format_diff()}")

    return "\n".join(lines)


def format_markdown(comparisons: list[TestComparison], summary: dict, baseline_name: str, current_name: str) -> str:
    """Format comparison as Markdown (gist-friendly)."""
    lines = []

    lines.append(f"# TinyCC Profile Comparison")
    lines.append(f"")
    lines.append(f"**Baseline:** {baseline_name}")
    lines.append(f"**Current:** {current_name}")
    lines.append(f"**Generated:** {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    lines.append(f"")

    # Summary table
    lines.append(f"## Summary")
    lines.append(f"")
    lines.append(f"| Metric | Baseline | Current | Change |")
    lines.append(f"|--------|----------|---------|--------|")
    lines.append(f"| Tests Passed | {summary['both_passed']} | {summary['both_passed']} | - |")

    if "max_heap_baseline_kb" in summary:
        sign = "+" if summary["heap_diff_pct"] > 0 else ""
        emoji = ":red_circle:" if summary["heap_diff_pct"] > 5 else (":green_circle:" if summary["heap_diff_pct"] < -5 else ":white_circle:")
        lines.append(f"| Max Heap Peak | {summary['max_heap_baseline_kb']} KB | {summary['max_heap_current_kb']} KB | {sign}{summary['heap_diff_pct']:.1f}% {emoji} |")

    if "max_rss_baseline_kb" in summary:
        sign = "+" if summary["rss_diff_pct"] > 0 else ""
        emoji = ":red_circle:" if summary["rss_diff_pct"] > 5 else (":green_circle:" if summary["rss_diff_pct"] < -5 else ":white_circle:")
        lines.append(f"| Max RSS | {summary['max_rss_baseline_kb']} KB | {summary['max_rss_current_kb']} KB | {sign}{summary['rss_diff_pct']:.1f}% {emoji} |")

    sign = "+" if summary["time_diff_pct"] > 0 else ""
    emoji = ":red_circle:" if summary["time_diff_pct"] > 10 else (":green_circle:" if summary["time_diff_pct"] < -10 else ":white_circle:")
    lines.append(f"| Total Compile Time | {summary['total_time_baseline_s']:.2f}s | {summary['total_time_current_s']:.2f}s | {sign}{summary['time_diff_pct']:.1f}% {emoji} |")

    sign = "+" if summary["size_diff_pct"] > 0 else ""
    emoji = ":red_circle:" if summary["size_diff_pct"] > 5 else (":green_circle:" if summary["size_diff_pct"] < -5 else ":white_circle:")
    lines.append(f"| Total Binary Size | {summary['total_size_baseline_b']} B | {summary['total_size_current_b']} B | {sign}{summary['size_diff_pct']:.1f}% {emoji} |")

    # Significant changes
    significant = []
    for c in comparisons:
        if c.heap_peak and c.heap_peak.is_significant:
            significant.append((c.test_name, "Heap", c.heap_peak))
        if c.max_rss and c.max_rss.is_significant:
            significant.append((c.test_name, "RSS", c.max_rss))
        if c.binary_size and c.binary_size.is_significant:
            significant.append((c.test_name, "Size", c.binary_size))

    if significant:
        lines.append(f"")
        lines.append(f"## Significant Changes (>5%)")
        lines.append(f"")
        lines.append(f"| Test | Metric | Baseline | Current | Change |")
        lines.append(f"|------|--------|----------|---------|--------|")
        for test, metric, comp in sorted(significant, key=lambda x: -abs(x[2].diff_pct))[:20]:
            emoji = ":green_circle:" if comp.is_better else ":red_circle:"
            lines.append(f"| {test} | {metric} | {comp.baseline:.0f} | {comp.current:.0f} | {comp.format_diff()} {emoji} |")

    # Full table (collapsed)
    lines.append(f"")
    lines.append(f"<details>")
    lines.append(f"<summary>Full Results ({len(comparisons)} tests)</summary>")
    lines.append(f"")
    lines.append(f"| Test | Heap (KB) | RSS (KB) | Size (B) | Time (s) |")
    lines.append(f"|------|-----------|----------|----------|----------|")
    for c in comparisons:
        if c.baseline_success and c.current_success:
            heap_str = f"{c.heap_peak.current:.0f}" if c.heap_peak else "-"
            rss_str = f"{c.max_rss.current:.0f}" if c.max_rss else "-"
            size_str = f"{c.binary_size.current:.0f}" if c.binary_size else "-"
            time_str = f"{c.compile_time.current:.3f}" if c.compile_time else "-"
            lines.append(f"| {c.test_name} | {heap_str} | {rss_str} | {size_str} | {time_str} |")
    lines.append(f"")
    lines.append(f"</details>")

    return "\n".join(lines)


def format_html(comparisons: list[TestComparison], summary: dict, baseline_name: str, current_name: str) -> str:
    """Format comparison as self-contained HTML with charts."""

    # Prepare chart data
    chart_labels = []
    heap_baseline = []
    heap_current = []
    size_baseline = []
    size_current = []

    for c in comparisons[:50]:  # Limit to 50 for readability
        if c.baseline_success and c.current_success:
            chart_labels.append(c.test_name)
            heap_baseline.append(c.heap_peak.baseline if c.heap_peak else 0)
            heap_current.append(c.heap_peak.current if c.heap_peak else 0)
            size_baseline.append(c.binary_size.baseline if c.binary_size else 0)
            size_current.append(c.binary_size.current if c.binary_size else 0)

    html = f'''<!DOCTYPE html>
<html>
<head>
    <title>TinyCC Profile Comparison</title>
    <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
    <style>
        body {{ font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; margin: 20px; background: #f5f5f5; }}
        .container {{ max-width: 1200px; margin: 0 auto; background: white; padding: 20px; border-radius: 8px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }}
        h1 {{ color: #333; }}
        .summary {{ display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); gap: 15px; margin: 20px 0; }}
        .stat {{ background: #f8f9fa; padding: 15px; border-radius: 8px; text-align: center; }}
        .stat-value {{ font-size: 24px; font-weight: bold; color: #333; }}
        .stat-label {{ font-size: 12px; color: #666; margin-top: 5px; }}
        .stat-change {{ font-size: 14px; margin-top: 5px; }}
        .positive {{ color: #dc3545; }}
        .negative {{ color: #28a745; }}
        .neutral {{ color: #6c757d; }}
        .chart-container {{ margin: 20px 0; }}
        table {{ width: 100%; border-collapse: collapse; margin: 20px 0; }}
        th, td {{ padding: 10px; text-align: left; border-bottom: 1px solid #ddd; }}
        th {{ background: #f8f9fa; }}
        .meta {{ color: #666; font-size: 14px; margin-bottom: 20px; }}
    </style>
</head>
<body>
    <div class="container">
        <h1>TinyCC Profile Comparison</h1>
        <div class="meta">
            <strong>Baseline:</strong> {baseline_name}<br>
            <strong>Current:</strong> {current_name}<br>
            <strong>Generated:</strong> {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}
        </div>

        <div class="summary">
            <div class="stat">
                <div class="stat-value">{summary['both_passed']}/{summary['total_tests']}</div>
                <div class="stat-label">Tests Passed</div>
            </div>
            {"".join(f'''
            <div class="stat">
                <div class="stat-value">{summary.get('max_heap_current_kb', 0)} KB</div>
                <div class="stat-label">Max Heap Peak</div>
                <div class="stat-change {'positive' if summary.get('heap_diff_pct', 0) > 5 else 'negative' if summary.get('heap_diff_pct', 0) < -5 else 'neutral'}">
                    {"+" if summary.get('heap_diff_pct', 0) > 0 else ""}{summary.get('heap_diff_pct', 0):.1f}%
                </div>
            </div>
            ''' if 'max_heap_current_kb' in summary else '')}
            {"".join(f'''
            <div class="stat">
                <div class="stat-value">{summary.get('max_rss_current_kb', 0)} KB</div>
                <div class="stat-label">Max RSS</div>
                <div class="stat-change {'positive' if summary.get('rss_diff_pct', 0) > 5 else 'negative' if summary.get('rss_diff_pct', 0) < -5 else 'neutral'}">
                    {"+" if summary.get('rss_diff_pct', 0) > 0 else ""}{summary.get('rss_diff_pct', 0):.1f}%
                </div>
            </div>
            ''' if 'max_rss_current_kb' in summary else '')}
            <div class="stat">
                <div class="stat-value">{summary['total_time_current_s']:.2f}s</div>
                <div class="stat-label">Total Compile Time</div>
                <div class="stat-change {'positive' if summary['time_diff_pct'] > 10 else 'negative' if summary['time_diff_pct'] < -10 else 'neutral'}">
                    {"+" if summary['time_diff_pct'] > 0 else ""}{summary['time_diff_pct']:.1f}%
                </div>
            </div>
            <div class="stat">
                <div class="stat-value">{summary['total_size_current_b'] / 1024:.1f} KB</div>
                <div class="stat-label">Total Binary Size</div>
                <div class="stat-change {'positive' if summary['size_diff_pct'] > 5 else 'negative' if summary['size_diff_pct'] < -5 else 'neutral'}">
                    {"+" if summary['size_diff_pct'] > 0 else ""}{summary['size_diff_pct']:.1f}%
                </div>
            </div>
        </div>

        <div class="chart-container">
            <h2>Heap Memory Usage</h2>
            <canvas id="heapChart" height="100"></canvas>
        </div>

        <div class="chart-container">
            <h2>Binary Size</h2>
            <canvas id="sizeChart" height="100"></canvas>
        </div>

        <h2>All Results</h2>
        <table>
            <tr>
                <th>Test</th>
                <th>Heap Peak (KB)</th>
                <th>Max RSS (KB)</th>
                <th>Binary Size (B)</th>
                <th>Compile Time (s)</th>
            </tr>
            {"".join(f'''
            <tr>
                <td>{c.test_name}</td>
                <td>{f"{c.heap_peak.current:.0f}" if c.heap_peak else "-"}</td>
                <td>{f"{c.max_rss.current:.0f}" if c.max_rss else "-"}</td>
                <td>{f"{c.binary_size.current:.0f}" if c.binary_size else "-"}</td>
                <td>{f"{c.compile_time.current:.3f}" if c.compile_time else "-"}</td>
            </tr>
            ''' for c in comparisons if c.baseline_success and c.current_success)}
        </table>
    </div>

    <script>
        const labels = {json.dumps(chart_labels)};

        new Chart(document.getElementById('heapChart'), {{
            type: 'bar',
            data: {{
                labels: labels,
                datasets: [
                    {{ label: 'Baseline', data: {json.dumps(heap_baseline)}, backgroundColor: 'rgba(54, 162, 235, 0.5)' }},
                    {{ label: 'Current', data: {json.dumps(heap_current)}, backgroundColor: 'rgba(255, 99, 132, 0.5)' }}
                ]
            }},
            options: {{ responsive: true, scales: {{ y: {{ beginAtZero: true, title: {{ display: true, text: 'KB' }} }} }} }}
        }});

        new Chart(document.getElementById('sizeChart'), {{
            type: 'bar',
            data: {{
                labels: labels,
                datasets: [
                    {{ label: 'Baseline', data: {json.dumps(size_baseline)}, backgroundColor: 'rgba(54, 162, 235, 0.5)' }},
                    {{ label: 'Current', data: {json.dumps(size_current)}, backgroundColor: 'rgba(255, 99, 132, 0.5)' }}
                ]
            }},
            options: {{ responsive: true, scales: {{ y: {{ beginAtZero: true, title: {{ display: true, text: 'Bytes' }} }} }} }}
        }});
    </script>
</body>
</html>'''

    return html


def save_baseline(profile_path: Path, name: str):
    """Save a profile as a named baseline."""
    BASELINES_DIR.mkdir(parents=True, exist_ok=True)

    # Load and copy profile
    data = load_profile(profile_path)

    # Add metadata
    baseline = {
        "name": name,
        "created": datetime.now().isoformat(),
        "source": str(profile_path),
        "results": data,
    }

    baseline_path = BASELINES_DIR / f"{name}.json"
    with open(baseline_path, 'w') as f:
        json.dump(baseline, f, indent=2)

    print(f"Saved baseline '{name}' to {baseline_path}")


def load_baseline(name: str) -> tuple[dict, str]:
    """Load a named baseline."""
    baseline_path = BASELINES_DIR / f"{name}.json"
    if not baseline_path.exists():
        raise FileNotFoundError(f"Baseline '{name}' not found. Available: {list_baselines()}")

    with open(baseline_path) as f:
        data = json.load(f)

    return data["results"], f"{name} ({data['created'][:10]})"


def list_baselines() -> list[str]:
    """List available baselines."""
    if not BASELINES_DIR.exists():
        return []
    return [p.stem for p in BASELINES_DIR.glob("*.json")]


def run_cmd(
    cmd: list[str],
    cwd: Path = None,
    check: bool = True,
    input_text: Optional[str] = None,
) -> subprocess.CompletedProcess:
    """Run a command and return result."""
    print(f"  $ {' '.join(cmd)}")
    result = subprocess.run(
        cmd,
        cwd=cwd,
        input=input_text,
        capture_output=True,
        text=True,
    )
    if check and result.returncode != 0:
        print(f"Command failed: {result.stderr}")
        raise subprocess.CalledProcessError(result.returncode, cmd, result.stdout, result.stderr)
    return result


def is_workspace_ref(ref: str) -> bool:
    return ref.strip().lower() == "workspace"


def copy_untracked_files(repo_path: Path, dest_path: Path) -> int:
    """Copy untracked files from repo_path into dest_path."""
    result = run_cmd(
        ["git", "ls-files", "--others", "--exclude-standard", "-z"],
        cwd=repo_path,
    )
    raw = result.stdout
    if not raw:
        return 0

    count = 0
    for rel in raw.split("\0"):
        if not rel:
            continue
        src = repo_path / rel
        dst = dest_path / rel
        if src.is_dir():
            continue
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
        count += 1
    return count


def create_workspace_snapshot(
    worktree_path: Path,
    base_ref: str = "HEAD",
    repo_path: Path = REPO_ROOT,
) -> None:
    """Create a worktree at base_ref and apply current working-tree changes on top."""
    create_worktree(base_ref, worktree_path, repo_path=repo_path)

    patch = run_cmd(["git", "diff", base_ref], cwd=repo_path).stdout
    if patch.strip():
        # Apply the patch inside the snapshot worktree.
        run_cmd(["git", "apply", "-"], cwd=worktree_path, input_text=patch)

    copied = copy_untracked_files(repo_path, worktree_path)
    if copied:
        print(f"  Copied {copied} untracked file(s) into workspace snapshot")


def get_git_short_hash(ref: str, repo_path: Path = REPO_ROOT) -> str:
    """Get short hash for a git ref."""
    result = run_cmd(["git", "rev-parse", "--short", ref], cwd=repo_path)
    return result.stdout.strip()


def get_git_commit_info(ref: str, repo_path: Path = REPO_ROOT) -> dict:
    """Get commit info for a git ref."""
    # Get hash
    hash_result = run_cmd(["git", "rev-parse", "--short", ref], cwd=repo_path)
    short_hash = hash_result.stdout.strip()

    # Get full hash
    full_hash_result = run_cmd(["git", "rev-parse", ref], cwd=repo_path)
    full_hash = full_hash_result.stdout.strip()

    # Get commit subject
    subject_result = run_cmd(["git", "log", "-1", "--format=%s", ref], cwd=repo_path)
    subject = subject_result.stdout.strip()

    # Get commit date
    date_result = run_cmd(["git", "log", "-1", "--format=%ci", ref], cwd=repo_path)
    date = date_result.stdout.strip()

    return {
        "ref": ref,
        "short_hash": short_hash,
        "full_hash": full_hash,
        "subject": subject,
        "date": date,
    }


def create_worktree(ref: str, worktree_path: Path, repo_path: Path = REPO_ROOT) -> None:
    """Create a git worktree for the given ref."""
    # Remove existing worktree if present
    if worktree_path.exists():
        print(f"Removing existing worktree at {worktree_path}")
        run_cmd(["git", "worktree", "remove", "--force", str(worktree_path)], cwd=repo_path, check=False)
        if worktree_path.exists():
            shutil.rmtree(worktree_path)

    print(f"Creating worktree for {ref} at {worktree_path}")
    run_cmd(["git", "worktree", "add", "--detach", str(worktree_path), ref], cwd=repo_path)


def remove_worktree(worktree_path: Path, repo_path: Path = REPO_ROOT) -> None:
    """Remove a git worktree."""
    if worktree_path.exists():
        print(f"Removing worktree at {worktree_path}")
        run_cmd(["git", "worktree", "remove", "--force", str(worktree_path)], cwd=repo_path, check=False)
        if worktree_path.exists():
            shutil.rmtree(worktree_path)


def build_tinycc(source_path: Path, build_path: Path = None, *, debug: bool = False) -> Path:
    """Build TinyCC from source (in-tree build), returns path to armv8m-tcc binary."""
    print(f"\nBuilding TinyCC from {source_path}")

    # TinyCC requires in-tree build, so we build directly in the source directory
    # The build_path parameter is kept for API compatibility but not used

    # Configure
    configure_script = source_path / "configure"
    if not configure_script.exists():
        raise FileNotFoundError(f"configure script not found at {configure_script}")

    print("  Configuring...")
    configure_cmd = ["./configure", "--enable-cross", "--enable-O2"]
    if debug:
        configure_cmd.append("--debug")
    run_cmd(configure_cmd, cwd=source_path)

    # Build
    print("  Building...")
    nproc = os.cpu_count() or 4
    run_cmd(["make", f"-j{nproc}"], cwd=source_path)

    # Return path to armv8m-tcc binary (cross compiler for ARM Cortex-M)
    tcc_path = source_path / "armv8m-tcc"
    if not tcc_path.exists():
        raise FileNotFoundError(f"armv8m-tcc binary not found at {tcc_path}")

    return tcc_path


def run_profile_suite(tcc_path: Path, output_dir: Path, profiler: str = "heaptrack",
                      limit: int = 0, cflags: str = "") -> Path:
    """Run profile_suite.py with a specific tcc binary, returns path to summary.json."""
    print(f"\nRunning profile suite with {tcc_path}")
    print(f"  Output: {output_dir}")

    output_dir.mkdir(parents=True, exist_ok=True)

    # Build the command
    cmd = [
        sys.executable,
        str(CURRENT_DIR / "profile_suite.py"),
        "--output-dir", str(output_dir),
        "--profiler", profiler,
        "--compiler", str(tcc_path),
    ]

    if limit > 0:
        cmd.extend(["--limit", str(limit)])

    if cflags:
        cmd.extend(["--cflags", cflags])

    print(f"  $ {' '.join(cmd)}")
    result = subprocess.run(cmd, cwd=CURRENT_DIR)

    if result.returncode != 0:
        print(f"Warning: profile_suite exited with code {result.returncode}")

    summary_path = output_dir / "summary.json"
    if not summary_path.exists():
        raise FileNotFoundError(f"Profile summary not found at {summary_path}")

    return summary_path


def git_compare(baseline_ref: str, current_ref: str = "HEAD",
                profiler: str = "heaptrack", limit: int = 0, cflags: str = "",
                output_dir: Path = None, keep_worktrees: bool = False) -> tuple[dict, dict, str, str]:
    """
    Compare profiling results between two git revisions.

    Returns: (baseline_data, current_data, baseline_name, current_name)
    """
    if output_dir is None:
        output_dir = CURRENT_DIR / "profile_results"

    # Get commit info
    if is_workspace_ref(baseline_ref):
        baseline_info = get_git_commit_info("HEAD")
    else:
        baseline_info = get_git_commit_info(baseline_ref)

    if is_workspace_ref(current_ref):
        current_info = get_git_commit_info("HEAD")
    else:
        current_info = get_git_commit_info(current_ref)

    print("=" * 70)
    print("Git Comparison")
    print("=" * 70)
    print(f"Baseline: {baseline_ref} ({baseline_info['short_hash']})")
    print(f"          {baseline_info['subject'][:60]}")
    print(f"Current:  {current_ref} ({current_info['short_hash']})")
    print(f"          {current_info['subject'][:60]}")
    print("=" * 70)

    # Create temporary directory for worktrees and builds
    with tempfile.TemporaryDirectory(prefix="tcc_profile_") as tmpdir:
        tmpdir = Path(tmpdir)

        # --- Build and profile baseline ---
        print("\n" + "=" * 70)
        print(f"PHASE 1: Building and profiling baseline ({baseline_ref})")
        print("=" * 70)

        baseline_worktree = tmpdir / "baseline_src"
        baseline_dir_name = f"baseline_{baseline_info['short_hash']}" + ("_workspace" if is_workspace_ref(baseline_ref) else "")
        baseline_profile_dir = output_dir / baseline_dir_name

        if is_workspace_ref(baseline_ref):
            create_workspace_snapshot(baseline_worktree, base_ref="HEAD")
        else:
            create_worktree(baseline_ref, baseline_worktree)
        baseline_tcc = build_tinycc(baseline_worktree, debug=(profiler == "callgrind"))
        baseline_summary = run_profile_suite(
            baseline_tcc, baseline_profile_dir,
            profiler=profiler, limit=limit, cflags=cflags
        )
        baseline_data = load_profile(baseline_summary)

        if not keep_worktrees:
            remove_worktree(baseline_worktree)

        # --- Build and profile current ---
        print("\n" + "=" * 70)
        print(f"PHASE 2: Building and profiling current ({current_ref})")
        print("=" * 70)

        # Always build from a clean directory. If current_ref is "workspace",
        # snapshot the working tree state into a temporary worktree.
        current_worktree = tmpdir / "current_src"
        if is_workspace_ref(current_ref):
            create_workspace_snapshot(current_worktree, base_ref="HEAD")
        else:
            create_worktree(current_ref, current_worktree)

        current_dir_name = f"current_{current_info['short_hash']}" + ("_workspace" if is_workspace_ref(current_ref) else "")
        current_profile_dir = output_dir / current_dir_name

        current_tcc = build_tinycc(current_worktree, debug=(profiler == "callgrind"))
        current_summary = run_profile_suite(
            current_tcc, current_profile_dir,
            profiler=profiler, limit=limit, cflags=cflags
        )
        current_data = load_profile(current_summary)

        if not keep_worktrees:
            remove_worktree(current_worktree)

    baseline_name = f"{baseline_ref} ({baseline_info['short_hash']})"
    if is_workspace_ref(baseline_ref):
        baseline_name = f"workspace (based on HEAD {baseline_info['short_hash']})"

    current_name = f"{current_ref} ({current_info['short_hash']})"
    if is_workspace_ref(current_ref):
        current_name = f"workspace (based on HEAD {current_info['short_hash']})"

    return baseline_data, current_data, baseline_name, current_name


def upload_gist(content: str, filename: str, description: str, public: bool = False) -> Optional[str]:
    """
    Upload content to GitHub Gist.

    Tries gh CLI first, falls back to API with GITHUB_TOKEN.
    Returns the gist URL or None on failure.
    """
    import tempfile
    import os

    # Try gh CLI first
    try:
        with tempfile.NamedTemporaryFile(mode='w', suffix='.md', delete=False) as f:
            f.write(content)
            temp_path = f.name

        cmd = ["gh", "gist", "create", temp_path, "-d", description]
        if public:
            cmd.append("--public")

        result = subprocess.run(cmd, capture_output=True, text=True)
        os.unlink(temp_path)

        if result.returncode == 0:
            # gh outputs the URL
            url = result.stdout.strip()
            return url
    except FileNotFoundError:
        pass  # gh not installed

    # Fall back to GitHub API
    token = os.environ.get("GITHUB_TOKEN")
    if not token:
        print("Error: Neither 'gh' CLI nor GITHUB_TOKEN available for gist upload")
        return None

    import urllib.request

    payload = {
        "description": description,
        "public": public,
        "files": {
            filename: {"content": content}
        }
    }

    req = urllib.request.Request(
        "https://api.github.com/gists",
        data=json.dumps(payload).encode(),
        headers={
            "Authorization": f"token {token}",
            "Accept": "application/vnd.github.v3+json",
            "Content-Type": "application/json",
        },
        method="POST"
    )

    try:
        with urllib.request.urlopen(req) as response:
            data = json.loads(response.read())
            return data.get("html_url")
    except Exception as e:
        print(f"Error uploading gist: {e}")
        return None


def main():
    parser = argparse.ArgumentParser(
        description="Compare TinyCC profile results",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Git comparison examples:
  # Compare a specific commit/tag against current HEAD
  python profile_compare.py --git-compare v0.9.27

    # Compare a commit/tag against your current workspace (uncommitted changes)
    python profile_compare.py --git-compare HEAD --git-current workspace

  # Compare two specific commits
  python profile_compare.py --git-compare abc123 --git-current def456

  # Quick comparison with limited tests
  python profile_compare.py --git-compare v0.9.27 --limit 10

  # Use time profiler instead of heaptrack
  python profile_compare.py --git-compare v0.9.27 --profiler time
""")
    parser.add_argument("baseline", nargs="?", help="Baseline profile JSON file")
    parser.add_argument("current", nargs="?", help="Current profile JSON file")
    parser.add_argument("--save-baseline", metavar="FILE", help="Save profile as baseline")
    parser.add_argument("--name", help="Name for saved baseline")
    parser.add_argument("--load-baseline", metavar="NAME", help="Load named baseline for comparison")
    parser.add_argument("--list-baselines", action="store_true", help="List saved baselines")
    parser.add_argument("--markdown", "-m", metavar="FILE", help="Output Markdown report")
    parser.add_argument("--html", metavar="FILE", help="Output HTML report")
    parser.add_argument("--gist", action="store_true", help="Upload report to GitHub Gist")
    parser.add_argument("--gist-public", action="store_true", help="Make gist public (default: secret)")

    # Git comparison options
    parser.add_argument("--git-compare", "-g", metavar="REF",
                        help="Compare against a git ref (tag/branch/commit). "
                             "Builds that revision, runs profiling, then compares with current HEAD.")
    parser.add_argument("--git-current", metavar="REF", default="HEAD",
                        help="Git ref to use as 'current' (default: HEAD). "
                            "Special value: 'workspace' uses your current working tree (uncommitted changes) by snapshotting it into a temp build dir.")
    default_profiler = "time" if sys.platform == "darwin" else "heaptrack"
    profiler_choices = ["heaptrack", "callgrind", "time", "perf"]
    if sys.platform == "darwin":
        profiler_choices.extend(["xctrace", "xcprofile"])  # alias for xctrace
    parser.add_argument("--profiler", "-p", choices=profiler_choices, default=default_profiler,
                        help=f"Profiler tool to use for git comparison (default: {default_profiler})")
    parser.add_argument("--xcprofile", action="store_true",
                        help="macOS convenience switch: same as --profiler xctrace")
    parser.add_argument("--limit", "-n", type=int, default=0,
                        help="Limit number of tests to run (0 = all)")
    parser.add_argument("--cflags", type=str, default="",
                        help="Additional CFLAGS to pass to the compiler")
    parser.add_argument("--output-dir", "-o", type=Path, default=None,
                        help="Output directory for profile results")

    args = parser.parse_args()

    if args.profiler == "xcprofile":
        args.profiler = "xctrace"
    if args.xcprofile:
        if sys.platform != "darwin":
            raise SystemExit("--xcprofile is macOS-only")
        args.profiler = "xctrace"

    # Backwards-compatible convenience: allow `profile_compare.py -g <ref> workspace`
    # to mean `--git-current workspace`.
    if args.git_compare and args.baseline and not args.current and args.baseline.strip().lower() == "workspace":
        args.git_current = "workspace"
        args.baseline = None

    # List baselines
    if args.list_baselines:
        baselines = list_baselines()
        if baselines:
            print("Available baselines:")
            for b in baselines:
                print(f"  - {b}")
        else:
            print("No baselines saved yet.")
        return 0

    # Save baseline
    if args.save_baseline:
        name = args.name or datetime.now().strftime("%Y%m%d_%H%M%S")
        save_baseline(Path(args.save_baseline), name)
        return 0

    # Git comparison mode
    if args.git_compare:
        try:
            baseline_data, current_data, baseline_name, current_name = git_compare(
                baseline_ref=args.git_compare,
                current_ref=args.git_current,
                profiler=args.profiler,
                limit=args.limit,
                cflags=args.cflags,
                output_dir=args.output_dir,
            )
        except Exception as e:
            print(f"Error during git comparison: {e}")
            import traceback
            traceback.print_exc()
            return 1
    else:
        # File comparison mode - need either (baseline + current) or (--load-baseline + current)
        current_path = args.current or args.baseline  # If only one positional, it's current

        if not current_path:
            parser.print_help()
            return 1

        # Load baseline
        if args.load_baseline:
            baseline_data, baseline_name = load_baseline(args.load_baseline)
            current_path = args.current or args.baseline
        elif args.baseline and args.current:
            baseline_data = load_profile(Path(args.baseline))
            baseline_name = Path(args.baseline).stem
            current_path = args.current
        else:
            parser.error("Either (baseline current), (--load-baseline current), or (--git-compare REF) required")
            return 1

        # Load current
        current_data = load_profile(Path(current_path))
        current_name = Path(current_path).stem

    # Compare
    comparisons = compare_profiles(baseline_data, current_data)
    summary = compute_summary(comparisons)

    # Output
    if args.markdown:
        md = format_markdown(comparisons, summary, baseline_name, current_name)
        Path(args.markdown).write_text(md)
        print(f"Markdown report saved to {args.markdown}")

    if args.html:
        html = format_html(comparisons, summary, baseline_name, current_name)
        Path(args.html).write_text(html)
        print(f"HTML report saved to {args.html}")

    if args.gist:
        md = format_markdown(comparisons, summary, baseline_name, current_name)
        description = f"TinyCC Profile: {baseline_name} vs {current_name}"
        filename = f"tinycc_profile_{datetime.now().strftime('%Y%m%d_%H%M%S')}.md"
        url = upload_gist(md, filename, description, public=args.gist_public)
        if url:
            print(f"Gist uploaded: {url}")

    # Always print console output
    print(format_console(comparisons, summary, baseline_name, current_name))

    return 0


if __name__ == "__main__":
    sys.exit(main())
