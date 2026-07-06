# DuckDB Trace Processor Baseline

Last updated: 2026-07-07

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

| Module (materialize all outputs) | SQLite | DuckDB | DuckDB 1T | Speedup | 1T speedup | Total rows |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `sched.with_context` | 262 ms | 131 ms | 137 ms | 2.0x | 1.9x | 384,623 |
| `sched.time_in_state` | 157 ms | 61 ms | 69 ms | 2.6x | 2.3x | 7,579 |
| `sched.thread_level_parallelism` | 1,224 ms | 175 ms | 355 ms | 7.0x | 3.4x | 500,992 |

Long trace (`trace_file.perfetto-trace`, 84 MB, median of 2, load ~4.8 s):

| Module (materialize all outputs) | SQLite | DuckDB | DuckDB 1T | Speedup | 1T speedup | Total rows |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `sched.with_context` | 3,438 ms | 1,538 ms | 1,591 ms | 2.2x | 2.2x | 5,063,305 |
| `sched.time_in_state` | 2,750 ms | 785 ms | 790 ms | 3.5x | 3.5x | 4,872 |
| `sched.thread_level_parallelism` | 23,406 ms | 2,288 ms | 9,474 ms | 10.2x | 2.5x | 12,133,502 |

Observations:

- The DuckDB lead widens with scale only when DuckDB can use multiple threads
  for the expensive operators. The window-function-heavy
  `thread_level_parallelism` module goes from 7.0x on the public trace to 10.2x
  on the long trace, but forcing `SET threads=1` drops it to 3.4x / 2.5x. Its
  `intervals_overlap_count!` computation (12M output rows on the long trace) is
  where DuckDB's vectorized and parallel execution pulls furthest ahead of
  SQLite's row-at-a-time execution.
- `sched.with_context` is the honest low end: it is a lazy view, so forcing its
  5M-row join to materialize is real per-row work for both engines and the lead
  stays a modest ~2x. The 1T result is nearly identical because the imported
  table scan is already single-threaded and the remaining hash joins are not the
  dominant cost.
- `sched.time_in_state` has tiny outputs (a few thousand rows), so its group time
  is essentially the module-load cost; DuckDB holds a steady ~2.5–3.5x, with
  little sensitivity to `SET threads=1`.

## Plan Comparison

SQLite plans were dumped with `EXPLAIN QUERY PLAN`; DuckDB plans were dumped
with `EXPLAIN ANALYZE`. For eager modules, the relevant query is the module's
defining `SELECT`, not the final benchmark `SELECT * FROM already_materialized`
copy.

Representative long-trace plan shapes:

| Query body | SQLite | DuckDB |
| --- | --- | --- |
| `sched.with_context` view body | Scans `sched`, then `thread`, then `process` as virtual tables in a nested-loop-style plan. | Scans `sched`, `thread`, and `process`, then uses an inner `HASH_JOIN(thread)` and left `HASH_JOIN(process)`; 0.525 s in `EXPLAIN ANALYZE`. |
| `sched_time_in_state_for_thread` CTAS body | Scans `thread_state` once for `summed`, materializes `total_dur` from a second `thread_state` scan, builds an automatic covering index on `total_dur(utid)`, and joins via that index with a Bloom filter. | Scans `thread_state` twice, computes both aggregates with `HASH_GROUP_BY`, then joins them with `HASH_JOIN`; 0.766 s in `EXPLAIN ANALYZE`. |
| `intervals_overlap_count!` runnable body | Scans `thread_state` separately for `_starts` and `_ends`, uses `UNION ALL`, uses a temp B-tree for `GROUP BY`, then another temp B-tree for `ORDER BY` before the window result. | Scans `thread_state` once, applies `state = 'R'` above `PERFETTO_DF_SCAN`, materializes a reusable CTE, reads it twice for starts/ends, then uses `HASH_GROUP_BY(ts)`, ordered `WINDOW`, and `ORDER_BY`; 0.886 s in `EXPLAIN ANALYZE`. |

The biggest planning difference is in `intervals_overlap_count!`: DuckDB avoids
the second base-table scan by reusing the filtered CTE, and uses hash/vectorized
operators for the merge and cumulative window. SQLite uses row-oriented virtual
table scans plus temp B-trees for grouping and ordering. This is why the
`sched.thread_level_parallelism` module is the largest DuckDB win.

The 1T ablation confirms that the DuckDB advantage is not just a different plan:
the same DuckDB plan gets much slower when execution parallelism is disabled.
The full long-trace `sched.thread_level_parallelism` module falls from 2,288 ms
to 9,474 ms with `SET threads=1`.

`DataframeScanInit` currently calls `duckdb_init_set_max_threads(info, 1)`, so
every `PERFETTO_DF_SCAN` source is a single-thread producer. It also only uses
projection pushdown: filters are not handed to the Perfetto dataframe planner.
The runnable-count plan shows this directly: DuckDB scans all 8,474,508
`thread_state` rows and applies `state = 'R'` above the scan, leaving 2,723,165
rows. That is real overhead, and filter pushdown would help filtered scans.
However, it is not what drags down the main benchmark result: the same scan is
~0.30 s, while the 1T penalty in the overlap query is seconds of downstream
group/window/sort work, and the full long-trace module loses 7.2 s when
multi-threaded DuckDB execution is disabled. The scan restriction caps how much
Perfetto-imported table reads can scale, but the measured regression is mainly
from disabling DuckDB parallelism after the scan.

The benchmark harness is `bench_duckdb_materialize.sh` at the repo root; it
builds the per-module SQL, runs each backend N times through
`trace_processor_shell query [--experimental-duckdb] --perf-file`, and reports
the median `t_query`. The DuckDB 1T column prepends `SET threads=1` before the
module SQL. Invoke it as
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
- `EXPLAIN QUERY PLAN <stmt>` bypasses the PerfettoSQL frontend in the SQLite
  path and is sent directly to SQLite, so the planner output is not stripped by
  PerfettoSQL parsing.
- `EXPLAIN ANALYZE <stmt>` bypasses the PerfettoSQL frontend in the DuckDB path
  and is sent directly to DuckDB, which returns the per-operator profile tree.
  `PRAGMA enable_profiling` also passes through as an alternative, but its
  output file only keeps the last statement.
- Leading DuckDB `SET ...;` statements also bypass the PerfettoSQL frontend so
  benchmark files can use `SET threads=1` before normal PerfettoSQL.
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
