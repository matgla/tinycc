# Per-revision optimizer metrics dashboard

Tracks code size, compile time, and RP2350 cycle counts per commit in a
Grafana dashboard backed by SQLite, so an SSA-migration commit's effect is a
graph, not a guess. The fuzz correctness sweep (O1/O2 divergence) is
deliberately **not** run automatically — it's expensive; run it by hand (see
below) and let `metrics/gate.py` judge the result.

## Layout

```
metrics/
  schema.sql              -- SQLite DDL (runs, correctness, codesize, compile_time, perf, accepted_divergence)
  record.py                -- collects one commit's metrics, upserts into metrics.db
  gate.py                   -- compares a run against its parent; --strict to fail the build
  grafana/
    docker-compose.yml
    provisioning/datasources/sqlite.yml
    provisioning/dashboards/dashboards.yml
    dashboards/optimizer_regressions.json
.github/workflows/metrics.yml
```

`record.py` reuses existing tooling rather than reimplementing it:
[scripts/regression_disasm.py](../scripts/regression_disasm.py) `run_csv_mode`
for code size, [tests/benchmarks/run_benchmark.py](../tests/benchmarks/run_benchmark.py)
for RP2350 perf, and [tests/fuzz/sweep_all.py](../tests/fuzz/sweep_all.py) for
the (manual) correctness sweep.

## One-time Pi setup

```bash
sudo mkdir -p /var/lib/tcc-metrics
sudo chown "$(whoami)" /var/lib/tcc-metrics
sqlite3 /var/lib/tcc-metrics/metrics.db < metrics/schema.sql
```

The DB lives outside the Actions workspace so `actions/checkout` never
touches it.

### Runner

This reuses the org-scoped self-hosted runner already registered for other
projects — no new runner to register. Two things to check:

1. The tinycc repo has access to that runner's runner group (org Settings ->
   Actions -> Runner groups).
2. The runner carries the `armv8m-rpi5` label (`.github/workflows/metrics.yml`
   targets `runs-on: [self-hosted, armv8m-rpi5]`). Add it by re-running the
   runner's `config.sh --labels armv8m-rpi5` (or editing labels via the GitHub
   UI) and restarting the runner service.

A self-hosted runner executes one job at a time, so this workflow queues
behind (or blocks) other repos' jobs on the same box while it runs, and vice
versa. That's accepted here since the job is bounded (codesize + compile-time
+ perf, no fuzz sweep) — see "What CI does" below.

Runner dependencies (installed once on the Pi, not per-run):
- `arm-none-eabi-gcc`/`objdump`/`nm` — required, for code size.
- Python 3 + `pip install paramiko` — required, for the RP2350 perf step.
- The RP2350 board wired to the Pi over USB, reachable via `127.0.0.1` SSH
  (`PERF_HOST`/`PERF_IDENTITY` in the workflow). If it's ever unplugged,
  `record.py` skips perf for that commit rather than failing.
- `qemu-system-arm` (mps2-an505) + the built newlib under
  `tests/ir_tests/qemu/mps2-an505` — **only** needed if you run the manual
  correctness sweep (below) on this same machine.

### Security note

`.github/workflows/metrics.yml` triggers on `pull_request`. Combined with a
self-hosted runner, that means PR code executes with access to this machine
and the attached hardware. Only safe as long as untrusted forks can't open
PRs against this repo. If that ever changes, either drop the `pull_request`
trigger or require maintainer approval for external-contributor workflow runs
(repo Settings -> Actions -> "Fork pull request workflows").

## What CI does

On every push and PR to `mob`: builds `armv8m-tcc`, then runs
`metrics/record.py --no-correctness` — code size (via `regression_disasm.py`),
compile time (the code-size corpus's wall time), and RP2350 perf if the board
answers. No fuzz sweep, no schedule/cron.

The gate step is present but a no-op until the `METRICS_GATE_ENABLED` repo
variable is set to `true` (Settings -> Actions -> Variables) — see "Gate
policy" below.

## Manual correctness sweeps

Run these by hand whenever you want a divergence data point (e.g. before/after
a legacy-pass retirement commit):

```bash
python3 metrics/record.py --db /var/lib/tcc-metrics/metrics.db --rev HEAD \
    --seed-lo 0 --seed-hi 1000 --mode prescan --jobs "$(nproc)"
```

Bump `--seed-hi` or use `--mode triage` (full-recall, slower, also
culprit-bisects) for a more thorough pass. Recording is idempotent — re-run
against the same commit any time to widen the band.

## Gate policy: track first, then block

`metrics/gate.py` compares a run against its parent commit's run:

```bash
python3 metrics/gate.py --db /var/lib/tcc-metrics/metrics.db --rev HEAD
```

Without `--strict` it only reports (exit 0 always) — safe to run before the
baseline is provably green. Add `--strict` to fail the build on a correctness
regression (a new divergent seed not seen in the parent) or a code-size
regression beyond `--codesize-tolerance-pct` (default 1%). compile time and
perf are reported but never gate — judge those by eye on the dashboard.

A pre-existing divergence (found once you finally run a wide correctness
sweep) is not a build failure — allowlist it:

```bash
python3 metrics/gate.py --db /var/lib/tcc-metrics/metrics.db \
    --accept ptr:olevels:12345 --reason "pre-existing, see docs/bugs.md"
```

Once a `--strict` run comes back clean, flip the CI gate on by setting the
`METRICS_GATE_ENABLED` repo variable to `true`.

## Grafana

```bash
cd metrics/grafana
docker compose up -d
```

Opens on `http://<pi>:3000`. The SQLite datasource and the
"TinyCC Optimizer Regressions" dashboard are provisioned automatically from
`provisioning/` and `dashboards/`. Panels: per-profile divergence, total
divergence, code-size ratio vs GCC, compile-time trend, RP2350 cycles, and a
"regressed since parent" table — the last one is the accept/reject signal for
each migration commit (see
[docs/plan_opt_predicate_framework.md](plan_opt_predicate_framework.md) and
the optimizer migration plan for how it's used).

## Backfilling history

Code size and compile time can be backfilled across past commits (correctness
and perf cannot — see `record.py`'s docstring for why):

```bash
python3 metrics/record.py --db /var/lib/tcc-metrics/metrics.db --backfill 100
```

This builds each of the last 100 first-parent commits into a throwaway tmpdir
(`regression_disasm.build_tcc_at_rev`) and measures against that binary. Slow
(a full `configure && make cross` per commit) — run it once, manually.
