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
    tcc-metrics-grafana.service  -- systemd unit, wraps podman-compose up/down
    provisioning/datasources/sqlite.yml
    provisioning/dashboards/dashboards.yml
    dashboards/optimizer_regressions.json
.github/workflows/ci.yml   -- build, build-and-test, build-and-measure, rp2350-perf
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

Only the `rp2350-perf` job needs the Pi — it reuses the org-scoped
self-hosted runner already registered for other projects, no new runner to
register. Two things to check:

1. The tinycc repo has access to that runner's runner group (org Settings ->
   Actions -> Runner groups).
2. The runner carries the `rpi5`/`pimoroni_pico_plus2` labels
   (`.github/workflows/ci.yml`'s `rp2350-perf` job targets
   `runs-on: [self-hosted, rpi5, pimoroni_pico_plus2]`). Add them via the
   runner's `config.sh --labels rpi5,pimoroni_pico_plus2` (or editing labels
   via the GitHub UI) and restarting the runner service.

`ci.yml` builds the cross compiler exactly once, in a dedicated `build` job
on a regular GitHub-hosted runner (`runs-on: ubuntu-latest`, same container
image `build-and-test` uses) — compiling on the Pi is much slower than a
cloud runner. `build` uploads `armv8m-tcc`/`armv8m-libtcc1.a` as a GitHub
Actions artifact; `build-and-measure` (`needs: build`) downloads it to
measure code size/compile time (no board needed) and uploads a scratch
metrics db of its own; `rp2350-perf` (`needs: build-and-measure`) downloads
both artifacts, so it never rebuilds tcc and never re-measures code size —
it only does what actually needs the board (running benchmarks over SSH),
then imports the earlier job's numbers into the persistent db via
`record.py --import-codesize-from` (see "What CI does" below).
`build-and-test` (the actual test suite) does **not** consume the `build`
artifact — `make test` depends on `cross`, which reaches through object
files and checksum/fp-libs/PCH stamp files, not just the final binary, so a
pre-built `armv8m-tcc` wouldn't save it a recompile; it stays fully
self-contained and runs in parallel with `build`.

A self-hosted runner executes one job at a time, so `rp2350-perf` still
queues behind (or blocks) other repos' jobs on the same box while it runs,
and vice versa — that's why its `concurrency: group: metrics-rpi5` is scoped
to just that job; the cloud `build`/`build-and-measure` jobs don't need to
queue behind Pi-bound work.

Runner dependencies (installed once on the Pi, not per-run):
- Python 3 + `pip install paramiko` — required, for the RP2350 perf step.
- The RP2350 board wired to the Pi over USB, reachable via `127.0.0.1` SSH
  (`PERF_HOST`/`PERF_IDENTITY` in the workflow). If it's ever unplugged,
  `record.py` skips perf for that commit rather than failing.
- `arm-none-eabi-gcc`/`objdump`/`nm` and `qemu-system-arm` (mps2-an505) +
  the built newlib under `tests/ir_tests/qemu/mps2-an505` — **only** needed
  if you run a manual full sweep (below) directly on the Pi; the automatic
  CI path no longer measures code size there, so these aren't required for
  `rp2350-perf` itself.

### Security note

`ci.yml`'s `rp2350-perf` job triggers on `pull_request`. Combined with a
self-hosted runner, that means PR code executes with access to this machine
and the attached hardware. Only safe as long as untrusted forks can't open
PRs against this repo. If that ever changes, either drop the `pull_request`
trigger or require maintainer approval for external-contributor workflow runs
(repo Settings -> Actions -> "Fork pull request workflows").

## What CI does

On every push and PR to `mob`, `ci.yml` runs four jobs (no schedule/cron, no
fuzz sweep in any of them):

1. `build` (cloud runner) builds `armv8m-tcc`/`armv8m-libtcc1.a` once and
   uploads them as an artifact.
2. `build-and-test` (cloud runner, runs in parallel with `build` -- does
   its own independent build, see the "Runner" section above for why it
   can't reuse `build`'s artifact) runs the full test suite.
3. `build-and-measure` (cloud runner, `needs: build`) downloads the tcc
   build, then runs `metrics/record.py --no-correctness` against a
   throwaway scratch db to measure code size (via `regression_disasm.py`)
   and compile time (the code-size corpus's wall time). It uploads the
   scratch db as an artifact.
4. `rp2350-perf` (self-hosted Pi, `needs: build-and-measure`) downloads the
   tcc build and the scratch db, imports the scratch db's
   codesize/compile-time rows into the persistent
   `/var/lib/tcc-metrics/metrics.db` via
   `record.py --import-codesize-from <scratch db>`, and measures RP2350
   perf if the board answers.

`build-and-measure` and `rp2350-perf` record under the same synthetic host
key (`METRICS_HOST: armv8m-metrics`, set at the workflow level) so they land
on **one** run row per commit instead of two — the db keys `runs` by
`(commit_sha, host)`, and both `gate.py` and the Grafana dashboard assume
one host owns every metric for a commit. `--import-codesize-from` is what
makes that work: it copies `codesize_rollup`/`codesize_func`/`compile_time`
rows for the matching commit from another metrics db instead of
recomputing them, so the `rp2350-perf` job's `upsert_run` (which always
clears a run's child tables before re-populating them) doesn't need to redo
`build-and-measure`'s measurement to fill them back in.

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

Grafana runs as a systemd-managed `podman-compose` stack, so it comes back on
its own after a reboot or crash instead of needing someone to SSH in and
re-run `podman-compose up -d`. Rootless Podman has no persistent daemon
equivalent to `dockerd` — `podman-compose` just shells out to `podman` — so
the unit only waits on the network, not a container-runtime service.

Grafana's compose file (`metrics/grafana/docker-compose.yml`) reads
`/var/lib/tcc-metrics/metrics.db` and needs to live somewhere stable — clone
the repo to a persistent path on the Pi (e.g. `/opt/tcc-metrics/tinycc`), not
the ephemeral `actions/checkout` workspace the CI job uses.

```bash
sudo git clone <this-repo-url> /opt/tcc-metrics/tinycc   # one-time, or pull to update
sudo cp /opt/tcc-metrics/tinycc/metrics/grafana/tcc-metrics-grafana.service \
    /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now tcc-metrics-grafana.service
```

Edit the unit's `WorkingDirectory` first if the clone isn't at
`/opt/tcc-metrics/tinycc`. Manage it like any other service:

```bash
systemctl status tcc-metrics-grafana   # is it up?
journalctl -u tcc-metrics-grafana      # compose up/down output
sudo systemctl restart tcc-metrics-grafana  # e.g. after editing docker-compose.yml
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
