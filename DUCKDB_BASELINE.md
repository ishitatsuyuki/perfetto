# DuckDB Trace Processor Baseline

Last updated: 2026-07-06

The current `--experimental-duckdb` integration is aimed at making selected
sched stdlib queries useful for benchmarking. Broad trace processor diff-test
compatibility is intentionally not the primary goal for this phase.

## Current Benchmark Target

The active target is sched query performance on `trace_file.perfetto-trace`.
Each benchmark uses one `INCLUDE PERFETTO MODULE ...` statement and one final
`SELECT count(*)` per shell invocation.

| Query | SQLite | DuckDB | Count |
| --- | ---: | ---: | ---: |
| `sched_with_thread_process` | 922 ms | 182 ms | 5,063,305 |
| `sched_time_in_state_for_thread` | 2,771 ms | 814 ms | 3,902 |
| `sched_percentage_of_time_in_state` | 2,765 ms | 809 ms | 970 |
| `sched_runnable_thread_count` | 23,179 ms | 2,237 ms | 5,241,893 |
| `sched_uninterruptible_sleep_thread_count` | 23,117 ms | 2,270 ms | 1,838,199 |
| `sched_active_cpu_count` | 22,846 ms | 2,217 ms | 5,053,410 |

Previously measured sched module probes:

| Query | SQLite | DuckDB | Count |
| --- | ---: | ---: | ---: |
| `sched_previous_runnable_on_thread` | 9,168 ms | 3,201 ms | 2,977,574 |
| `sched_latency_for_running_interval` | 10,838 ms | 5,254 ms | 2,977,573 |

Example command shape:

```sh
out/codex_duckdb/trace_processor_shell query --experimental-duckdb \
  -f /tmp/query.sql trace_file.perfetto-trace
```

Example query:

```sql
INCLUDE PERFETTO MODULE sched.thread_level_parallelism;
SELECT count(*) AS c FROM sched_runnable_thread_count;
```

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

## Active Gaps

- `CREATE PERFETTO FUNCTION` is skipped in the DuckDB path. This is enough for
  the current table/view benchmarks but not for queries that call those
  functions.
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
