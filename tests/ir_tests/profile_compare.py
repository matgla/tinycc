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

Output formats:
    - Console: colored diff table
    - Markdown: gist-friendly table
    - HTML: self-contained page with charts
    - Gist: auto-upload markdown to GitHub
"""

import argparse
import json
import subprocess
import sys
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Optional

CURRENT_DIR = Path(__file__).parent
BASELINES_DIR = CURRENT_DIR / "profile_baselines"


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
    lines.append(f"| Test | Heap (KB) | Size (B) | Time (s) |")
    lines.append(f"|------|-----------|----------|----------|")
    for c in comparisons:
        if c.baseline_success and c.current_success:
            heap_str = f"{c.heap_peak.current:.0f}" if c.heap_peak else "-"
            size_str = f"{c.binary_size.current:.0f}" if c.binary_size else "-"
            time_str = f"{c.compile_time.current:.3f}" if c.compile_time else "-"
            lines.append(f"| {c.test_name} | {heap_str} | {size_str} | {time_str} |")
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
                <th>Binary Size (B)</th>
                <th>Compile Time (s)</th>
            </tr>
            {"".join(f'''
            <tr>
                <td>{c.test_name}</td>
                <td>{f"{c.heap_peak.current:.0f}" if c.heap_peak else "-"}</td>
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
    parser = argparse.ArgumentParser(description="Compare TinyCC profile results")
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
    args = parser.parse_args()

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

    # Compare - need either (baseline + current) or (--load-baseline + current)
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
        parser.error("Either (baseline current) or (--load-baseline current) required")
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
