-- schema.sql -- per-revision optimizer-regression metrics store.
--
-- One SQLite file (default /var/lib/tcc-metrics/metrics.db on the Pi) written by
-- metrics/record.py and read by Grafana (frser-sqlite-datasource).
-- The x-axis for every dashboard panel is runs.commit_ts (committer unix time),
-- so the graphs are commit-indexed, not wall-clock-indexed.
--
-- Idempotency contract (see record.py): `runs` is UNIQUE(commit_sha, host);
-- child rows are DELETEd for a run_id and re-INSERTed inside one transaction, so
-- re-recording a commit is a clean replace, never a duplicate.
--
-- Apply with:  sqlite3 metrics.db < metrics/schema.sql   (safe to re-run).

-- NOT WAL: Grafana bind-mounts this file read-only (docker-compose.yml `:ro`),
-- and SQLite cannot open a WAL database read-only -- even a SELECT must write the
-- -wal/-shm sidecars, which fails with "attempt to write a readonly database".
-- Rollback-journal mode reads fine from read-only media; at a once-per-commit
-- write cadence the recorder (busy timeout 60s) and the dashboard never contend.
PRAGMA journal_mode = DELETE;
PRAGMA foreign_keys = ON;

-- One row per (commit, host).  parent_sha = first parent, used by the
-- "regressed since parent" panels.  host matters because perf (cycles) is
-- hardware-specific; correctness/codesize are host-independent but still keyed
-- by host so one DB can hold more than one runner.
CREATE TABLE IF NOT EXISTS runs (
    run_id        INTEGER PRIMARY KEY,
    commit_sha    TEXT    NOT NULL,
    parent_sha    TEXT,
    branch        TEXT    NOT NULL DEFAULT 'mob',
    author        TEXT,
    author_email  TEXT,
    subject       TEXT,
    commit_ts     INTEGER NOT NULL,       -- committer unix ts = graph x-axis
    run_ts        INTEGER NOT NULL,       -- when the recorder ran
    host          TEXT    NOT NULL,
    tcc_build_ok  INTEGER NOT NULL DEFAULT 1,
    wall_seconds  REAL,
    seed_lo       INTEGER,                -- correctness band actually swept
    seed_hi       INTEGER,
    mode          TEXT,                   -- 'prescan' | 'triage'
    trigger       TEXT,                   -- 'push' | 'schedule' | 'backfill' | 'manual'
    notes         TEXT,
    UNIQUE(commit_sha, host)
);
CREATE INDEX IF NOT EXISTS ix_runs_commit_ts ON runs(commit_ts);
CREATE INDEX IF NOT EXISTS ix_runs_sha       ON runs(commit_sha);

-- (1) O1/O2 correctness divergence, one row per (run, profile, oracle).
-- oracle='olevels'  -> tcc -O0/-O1/-O2/-Os self-consistency
-- oracle='vsgcc'    -> vs arm-none-eabi-gcc -O2 gold
-- gccbad_count      -> seeds where gcc -O0 != gcc -O2 (oracle-unreliable, quarantined)
-- low_recall        -> 1 for ptr/struct_byval under prescan (~80% recall caveat)
CREATE TABLE IF NOT EXISTS correctness (
    run_id          INTEGER NOT NULL REFERENCES runs(run_id) ON DELETE CASCADE,
    profile         TEXT    NOT NULL,
    oracle          TEXT    NOT NULL,
    divergent_count INTEGER NOT NULL DEFAULT 0,
    gccbad_count    INTEGER NOT NULL DEFAULT 0,
    seed_lo         INTEGER NOT NULL,
    seed_hi         INTEGER NOT NULL,
    mode            TEXT    NOT NULL,
    low_recall      INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY(run_id, profile, oracle)
);

-- Drill-down + gate input: the actual divergent seed ids.
CREATE TABLE IF NOT EXISTS correctness_seed (
    run_id  INTEGER NOT NULL REFERENCES runs(run_id) ON DELETE CASCADE,
    profile TEXT    NOT NULL,
    oracle  TEXT    NOT NULL,
    seed    INTEGER NOT NULL,
    PRIMARY KEY(run_id, profile, oracle, seed)
);

-- (2a) code-size ROLLUP -- always written, small (one row per (suite, opt) + a
-- '<total>' grand-total row per opt).  ratio = tcc_size / gcc_size at that opt.
-- opt is 'o0'|'o1'|'o2': TCC and GCC are both compiled at the SAME level, so
-- each series compares like-for-like (tcc -O1 vs gcc -O1, etc.).
CREATE TABLE IF NOT EXISTS codesize_rollup (
    run_id     INTEGER NOT NULL REFERENCES runs(run_id) ON DELETE CASCADE,
    suite      TEXT    NOT NULL,      -- '<total>' for the grand total
    opt        TEXT    NOT NULL,      -- 'o0' | 'o1' | 'o2'
    func_count INTEGER NOT NULL,
    tcc_size   INTEGER NOT NULL,
    gcc_size   INTEGER NOT NULL,
    ratio      REAL    NOT NULL,
    PRIMARY KEY(run_id, suite, opt)
);

-- (2b) code-size DETAIL -- per-function per opt; large (~thousands of rows/run
-- x 3 opts).  Grafana's persistent metrics.db should normally contain only
-- rollups; CI writes this table in a scratch db and record.py --detail-db syncs
-- it to /var/lib/tcc-metrics/codesize-detail.db for the standalone detail
-- viewer.
CREATE TABLE IF NOT EXISTS codesize_func (
    run_id   INTEGER NOT NULL REFERENCES runs(run_id) ON DELETE CASCADE,
    suite    TEXT    NOT NULL,
    test     TEXT    NOT NULL,
    function TEXT    NOT NULL,
    opt      TEXT    NOT NULL,        -- 'o0' | 'o1' | 'o2'
    tcc_size INTEGER NOT NULL,
    gcc_size INTEGER NOT NULL,
    ratio    REAL    NOT NULL,
    PRIMARY KEY(run_id, suite, test, function, opt)
);

-- (3) compile time.  scope='codesize_corpus_o0'|'o1'|'o2' is the wall time of the
-- code-size corpus compile at that TCC opt level (deterministic, no hardware);
-- n_units = function count for throughput normalization.
CREATE TABLE IF NOT EXISTS compile_time (
    run_id  INTEGER NOT NULL REFERENCES runs(run_id) ON DELETE CASCADE,
    scope   TEXT    NOT NULL,
    seconds REAL    NOT NULL,
    n_units INTEGER,
    PRIMARY KEY(run_id, scope)
);

-- (4) RP2350 hardware perf, one row per (run, benchmark, compiler, opt_level).
CREATE TABLE IF NOT EXISTS perf (
    run_id          INTEGER NOT NULL REFERENCES runs(run_id) ON DELETE CASCADE,
    benchmark       TEXT    NOT NULL,
    compiler        TEXT    NOT NULL,     -- 'TCC' | 'GCC'
    opt_level       TEXT    NOT NULL,     -- 'o0' | 'o1' | 'o2'
    cycles_per_iter REAL    NOT NULL,
    build_text      INTEGER,
    build_data      INTEGER,
    build_bss       INTEGER,
    verify          TEXT,                 -- 'PASS'/'FAIL' from BenchmarkResult.verify
    PRIMARY KEY(run_id, benchmark, compiler, opt_level)
);

-- Gate allowlist (track-first -> block): pre-existing / accepted divergences the
-- gate must not fail on.  A row with a concrete `seed` accepts exactly that seed;
-- a row with seed IS NULL accepts a count baseline (`baseline`) for the profile.
CREATE TABLE IF NOT EXISTS accepted_divergence (
    profile   TEXT    NOT NULL,
    oracle    TEXT    NOT NULL,
    seed      INTEGER,
    baseline  INTEGER,
    reason    TEXT    NOT NULL,
    added_by  TEXT,
    added_ts  INTEGER NOT NULL,
    PRIMARY KEY(profile, oracle, seed)
);
