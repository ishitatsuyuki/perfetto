#!/usr/bin/env bash
# Benchmark DuckDB vs SQLite trace processor with FULL result materialization.
#
# Benchmarks are grouped by stdlib module, because a single INCLUDE builds every
# table that module defines. sched.thread_level_parallelism builds all three of
# its tables at INCLUDE time, and sched.time_in_state builds both of its tables,
# so timing them per-table would measure the same module load 3x / 2x. Each
# group here does ONE INCLUDE and then materializes every output the module
# exposes, so the three groups are three independent data points.
#
# Materialization is forced via CREATE PERFETTO TABLE ... AS SELECT *, which maps
# to a real DuckDB CREATE TABLE AS SELECT. Timing uses --perf-file (t_load,
# t_query); we report t_query in ms (median of N), excluding trace load.
set -euo pipefail

SHELL_BIN="out/codex_duckdb/trace_processor_shell"
TRACE="${1:-test/data/example_android_trace_30s.pb}"
RUNS="${2:-3}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# label|module|comma-separated output tables/views to materialize
SPECS=(
  "sched.with_context|sched.with_context|sched_with_thread_process"
  "sched.time_in_state|sched.time_in_state|sched_time_in_state_for_thread,sched_percentage_of_time_in_state"
  "sched.thread_level_parallelism|sched.thread_level_parallelism|sched_runnable_thread_count,sched_uninterruptible_sleep_thread_count,sched_active_cpu_count"
)

median_ms() { # reads ns values on stdin, prints integer ms median
  sort -n | awk '{a[NR]=$1} END{n=NR; if(n%2){m=a[(n+1)/2]}else{m=(a[n/2]+a[n/2+1])/2}; printf "%.0f\n", m/1000000}'
}

build_sql() { # $1=module $2=tables_csv $3=outfile
  { echo "INCLUDE PERFETTO MODULE $1;"
    local i=0 IFS=,
    for t in $2; do echo "CREATE PERFETTO TABLE r$i AS SELECT * FROM $t;"; i=$((i+1)); done
  } > "$3"
}

run_one() { # $1=engine_flag $2=sqlfile $3=thread_count_or_empty ; prints "t_load,t_query" ns
  local flag="$1" sqlf="$2" threads="${3:-}" perf="$WORK/perf.txt" run_sql="$sqlf"
  if [ -n "$threads" ]; then
    run_sql="$WORK/threaded.sql"
    { printf 'SET threads=%s;\n' "$threads"; cat "$sqlf"; } > "$run_sql"
  fi
  rm -f "$perf"
  # shellcheck disable=SC2086
  "$SHELL_BIN" query $flag --perf-file "$perf" -f "$run_sql" "$TRACE" >/dev/null 2>&1
  cat "$perf"
}

total_rows() { # $1=module $2=tables_csv ; sum of output row counts (via DuckDB)
  local cnt=0 n IFS=, f="$WORK/cnt.sql"
  for t in $2; do
    printf 'INCLUDE PERFETTO MODULE %s;\nCREATE PERFETTO TABLE result AS SELECT * FROM %s;\nSELECT count(*) AS c FROM result;\n' "$1" "$t" > "$f"
    n=$("$SHELL_BIN" query --experimental-duckdb -f "$f" "$TRACE" 2>/dev/null | tail -1)
    cnt=$((cnt + n))
  done
  echo "$cnt"
}

echo "Trace: $TRACE   (runs: $RUNS, median t_query)"
printf "%-32s | %10s | %10s | %10s | %8s | %9s | %12s\n" "module (materialize all outputs)" "SQLite ms" "DuckDB ms" "DuckDB 1T" "speedup" "1T speedup" "total rows"
printf -- '-%.0s' {1..111}; echo
load_reported=""
for spec in "${SPECS[@]}"; do
  label="${spec%%|*}"; rest="${spec#*|}"
  module="${rest%%|*}"; tables="${rest##*|}"
  sqlf="$WORK/${label//./_}.sql"
  build_sql "$module" "$tables" "$sqlf"
  rows=$(total_rows "$module" "$tables")

  sq=(); dk=(); dk1=(); loads=()
  for _ in $(seq "$RUNS"); do r=$(run_one "" "$sqlf"); sq+=("${r#*,}"); loads+=("${r%,*}"); done
  for _ in $(seq "$RUNS"); do r=$(run_one "--experimental-duckdb" "$sqlf"); dk+=("${r#*,}"); done
  for _ in $(seq "$RUNS"); do r=$(run_one "--experimental-duckdb" "$sqlf" 1); dk1+=("${r#*,}"); done
  sqm=$(printf '%s\n' "${sq[@]}" | median_ms)
  dkm=$(printf '%s\n' "${dk[@]}" | median_ms)
  dk1m=$(printf '%s\n' "${dk1[@]}" | median_ms)
  spd=$(awk -v s="$sqm" -v d="$dkm" 'BEGIN{ if(d>0) printf "%.1fx", s/d; else print "-" }')
  spd1=$(awk -v s="$sqm" -v d="$dk1m" 'BEGIN{ if(d>0) printf "%.1fx", s/d; else print "-" }')
  printf "%-32s | %10s | %10s | %10s | %8s | %9s | %12s\n" "$label" "$sqm" "$dkm" "$dk1m" "$spd" "$spd1" "$rows"
  [ -z "$load_reported" ] && load_reported=$(printf '%s\n' "${loads[@]}" | median_ms)
done
echo "(trace load, median: ${load_reported} ms, excluded from query times above)"
