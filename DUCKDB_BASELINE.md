# DuckDB Trace Processor Baseline

Last updated: 2026-07-06

The current `--experimental-duckdb` integration is aimed at making selected
sched stdlib queries useful for benchmarking. Broad trace processor diff-test
compatibility is intentionally not the primary goal for this phase.

## Benchmark Targets

The active public target is sched query performance on
`test/data/example_android_trace_30s.pb`. It has enough scheduler data to be a
useful checked-in baseline: 384,623 `sched` rows, 549,904 `thread_state` rows,
and 8 CPUs. For scale comparison we also run the longer local
`trace_file.perfetto-trace` (not checked in): 5,063,305 `sched` rows, 8,474,508
`thread_state` rows, and 16 CPUs.

Benchmarks are grouped by stdlib module, because a single `INCLUDE` builds every
table a module defines. `sched.thread_level_parallelism` builds all three of its
tables (`runnable`, `uninterruptible_sleep`, `active_cpu`) on every include, and
`sched.time_in_state` builds both of its tables, so timing them per-table would
just measure the same module load 3x / 2x. Each group therefore does one
`INCLUDE` and then materializes *every* output that module exposes:

```sql
INCLUDE PERFETTO MODULE sched.thread_level_parallelism;
CREATE PERFETTO TABLE r0 AS SELECT * FROM sched_runnable_thread_count;
CREATE PERFETTO TABLE r1 AS SELECT * FROM sched_uninterruptible_sleep_thread_count;
CREATE PERFETTO TABLE r2 AS SELECT * FROM sched_active_cpu_count;
```

`CREATE PERFETTO TABLE` maps to a plain DuckDB `CREATE TABLE ... AS SELECT`, so
every output row is genuinely computed and stored in a real temp result table
(no `count(*)` shortcut that would let the optimizer skip building the rows).
Timing uses the shell's `--perf-file` output (`t_load,t_query`); we report
`t_query` in ms, excluding trace load. The three groups are three independent
data points. Of the three, only `sched.with_context` is a `PERFETTO VIEW` (built
lazily, so the join work happens at materialization time); the other two modules
are backed by `PERFETTO TABLE`s built eagerly at `INCLUDE`, so their group time
is dominated by the module load and the per-table `CREATE ... AS SELECT` is a
cheap copy.

Public trace (`test/data/example_android_trace_30s.pb`, median of 5, load ~0.4 s):

| Module (materialize all outputs) | SQLite | DuckDB | Speedup | Total rows |
| --- | ---: | ---: | ---: | ---: |
| `sched.with_context` | 263 ms | 133 ms | 2.0x | 384,623 |
| `sched.time_in_state` | 153 ms | 60 ms | 2.5x | 7,579 |
| `sched.thread_level_parallelism` | 1,206 ms | 178 ms | 6.8x | 500,992 |

Long trace (`trace_file.perfetto-trace`, 84 MB, median of 2, load ~4.8 s):

| Module (materialize all outputs) | SQLite | DuckDB | Speedup | Total rows |
| --- | ---: | ---: | ---: | ---: |
| `sched.with_context` | 3,546 ms | 1,558 ms | 2.3x | 5,063,305 |
| `sched.time_in_state` | 2,660 ms | 810 ms | 3.3x | 4,872 |
| `sched.thread_level_parallelism` | 23,175 ms | 2,278 ms | 10.2x | 12,133,502 |

Observations:

- The DuckDB lead widens with scale. The window-function-heavy
  `thread_level_parallelism` module goes from 6.8x on the public trace to 10.2x
  on the long trace — its `intervals_overlap_count!` computation (12M output
  rows) is where DuckDB's vectorized engine pulls furthest ahead of SQLite's
  row-at-a-time execution.
- `sched.with_context` is the honest low end: it is a lazy view, so forcing its
  5M-row join to materialize is real per-row work for both engines and the lead
  stays a modest ~2x.
- `sched.time_in_state` has tiny outputs (a few thousand rows), so its group time
  is essentially the module-load cost; DuckDB holds a steady ~2.5–3.3x.

The benchmark harness is `bench_duckdb_materialize.sh` at the repo root; it
builds the per-module SQL, runs each backend N times through
`trace_processor_shell query [--experimental-duckdb] --perf-file`, and reports
the median `t_query`. Invoke it as
`bash bench_duckdb_materialize.sh [trace] [runs]`.

## Current DuckDB Compatibility Notes

- The DuckDB path now runs PerfettoSQL parsing and preprocessing for the target
  modules, including exact `INCLUDE PERFETTO MODULE`, `CREATE PERFETTO TABLE`,
  `CREATE PERFETTO VIEW`, and `CREATE PERFETTO MACRO` handling.
- `sched.with_context` is a low-risk regression benchmark: it exercises typed
  view creation and joins over imported sched/thread/process tables.
- `sched.time_in_state` requires macro registration for `_case_for_state!`.
  It also needs a focused rewrite from `/ total_runtime AS other` to
  `/ max(total_runtime) AS other` because DuckDB enforces strict aggregate
  grouping where SQLite accepts the original expression.
- `sched.thread_level_parallelism` exercises `intervals_overlap_count!` from
  `intervals.overlap`, which expands to pure SQL with window functions.
- `intervals.overlap` includes `android.monitor_contention`, but the target
  thread-level parallelism queries only need the interval macros. The DuckDB
  path skips that transitive module for now to avoid unrelated unsupported
  function and span-join dependencies.
- DuckDB prelude SQL macros provide `trace_start()`, `trace_end()`, and
  `trace_dur()` for modules that reference trace bounds while loading.
- Non-final setup statement failures are returned immediately so failed CTAS
  statements are not hidden behind later missing-table errors.
- `EXPLAIN ANALYZE <stmt>` bypasses the PerfettoSQL frontend in the DuckDB
  path and is sent directly to DuckDB, which returns the per-operator profile
  tree. `PRAGMA enable_profiling` also passes through as an alternative, but
  its output file only keeps the last statement.
- Non-delegating SQL-defined `CREATE PERFETTO FUNCTION` statements are
  translated into DuckDB scalar/table macros on a best-effort basis. Function
  bodies that reference unsupported intrinsics are still skipped until called.

## Active Gaps

- Delegating `CREATE PERFETTO FUNCTION ... DELEGATES TO ...` aliases remain
  skipped because the DuckDB path does not yet register the C++ intrinsic
  targets they delegate to.
- `CREATE VIRTUAL TABLE ... USING SPAN_JOIN` and other virtual table modules
  are not supported.
- The compatibility rewrites are deliberately narrow and benchmark-driven.
  They are not a general SQLite-to-DuckDB SQL translation layer.
- Result formatting and overall diff-test pass rate remain secondary while the
  integration is evaluated for targeted benchmark value.

## Historical Diff-Test Snapshot

On 2026-07-03, the initial PoC was measured through a wrapper that injected
`--experimental-duckdb` into the trace processor diff-test runner. This snapshot
is useful only as historical context; several failures listed there have since
been addressed for the sched path.

| Bucket | Ran | Passed | Failed | Runtime |
| --- | ---: | ---: | ---: | ---: |
| `^Tables:` | 19 | 9 | 10 | 20.6s |
| `^(Smoke|SmokeJson|SmokeSchedEvents):` | 5 | 4 | 1 | 18.9s |
| `.*counter.*` | 59 | 25 | 34 | 132.1s |
| `^SchedParser:` | 4 | 0 | 4 | 1.6s |

The main failure classes at that point were missing PerfettoSQL preprocessing,
missing Perfetto scalar functions, unsupported virtual tables, SQLite/DuckDB
syntax differences, output formatting differences, ordering differences, and
multi-statement setup semantics.
