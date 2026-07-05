# DuckDB Trace Processor PoC Baseline

Date: 2026-07-03

This baseline measures the current `--experimental-duckdb` PoC by running
existing trace processor diff tests through a wrapper that injects the DuckDB
flag:

```sh
out/codex_duckdb/trace_processor_shell_duckdb
```

The wrapper forwards to `out/codex_duckdb/trace_processor_shell` with
`--experimental-duckdb` and sanitizes stderr to keep the Python diff-test
runner from aborting on non-UTF-8 trace health output.

## Commands

```sh
tools/diff_test_trace_processor.py --quiet --no-colors -j 1 \
  --name-filter '^Tables:' \
  out/codex_duckdb/trace_processor_shell_duckdb

tools/diff_test_trace_processor.py --quiet --no-colors -j 1 \
  --name-filter '^(Smoke|SmokeJson|SmokeSchedEvents):' \
  out/codex_duckdb/trace_processor_shell_duckdb

tools/diff_test_trace_processor.py --quiet --no-colors -j 1 \
  --name-filter '.*counter.*' \
  out/codex_duckdb/trace_processor_shell_duckdb

tools/diff_test_trace_processor.py --quiet --no-colors -j 1 \
  --name-filter '^SchedParser:' \
  out/codex_duckdb/trace_processor_shell_duckdb
```

## Results

| Bucket | Ran | Passed | Failed | Runtime |
| --- | ---: | ---: | ---: | ---: |
| `^Tables:` | 19 | 9 | 10 | 20.6s |
| `^(Smoke|SmokeJson|SmokeSchedEvents):` | 5 | 4 | 1 | 18.9s |
| `.*counter.*` | 59 | 25 | 34 | 132.1s |
| `^SchedParser:` | 4 | 0 | 4 | 1.6s |

## Failure Taxonomy

Most failures currently fall into these buckets:

- PerfettoSQL statements sent directly to DuckDB: `INCLUDE PERFETTO MODULE`,
  `CREATE PERFETTO TABLE`, `CREATE VIRTUAL TABLE`, table-function bang syntax.
- SQLite-specific syntax differences: `GLOB`, double-quoted string literals,
  `IS` in join predicates, comma-separated subqueries in a `FROM` clause.
- Missing Perfetto scalar functions: `extract_arg`, `extract_metadata`,
  `extract_metadata_for_machine`, `to_realtime`, `run_metric`.
- Result formatting differences: DuckDB emits names such as `count_star()`
  instead of SQLite's `count(*)` / `COUNT(*)`; some aggregate values are typed
  as strings after mixed-type import.
- Ordering differences where queries sort by a non-unique key and DuckDB
  returns a different tie order.
- Multi-statement handling differences: queries with setup statements can
  produce "Result rows were returned for multiples queries" through the current
  DuckDB path.

## Interpretation

The current PoC is useful for simple materialized table and view queries. The
next progress milestones are not primarily trace import fixes; they are
preprocessing and compatibility layers before DuckDB execution:

1. Run PerfettoSQL preprocessing before handing SQL to DuckDB.
2. Register or rewrite common Perfetto scalar functions.
3. Add a small SQLite-to-DuckDB syntax compatibility rewrite for `GLOB`,
   string literals, and simple `IS` predicates.
4. Normalize output column names and mixed-type import behavior.
5. Preserve SQLite's multi-statement semantics for setup-plus-final-select
   diff tests.
