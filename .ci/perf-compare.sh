#!/bin/bash
#
# Compare benchmark results against baseline thresholds.
#
# Usage: perf-compare.sh <mode> <results-json>
#
#   mode:          "vmpeak" or "valgrind"
#   results-json:  path to the JSON output from ovn-benchmark.sh --json
#
# Reads baseline thresholds from .ci/perf-baseline.json (relative to the
# repository root).  Exits non-zero if any metric exceeds its threshold
# plus the configured tolerance.
#
# When $GITHUB_STEP_SUMMARY is set, writes a Markdown results table to it.

set -e

MODE="$1"
RESULTS="$2"

if [ -z "$MODE" ] || [ -z "$RESULTS" ]; then
    echo "Usage: $0 <vmpeak|valgrind> <results.json>"
    exit 1
fi

if [ ! -f "$RESULTS" ]; then
    echo "Error: results file '$RESULTS' not found"
    exit 1
fi

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
BASELINE="$SCRIPT_DIR/perf-baseline.json"

if [ ! -f "$BASELINE" ]; then
    echo "Error: baseline file '$BASELINE' not found"
    exit 1
fi

# Parse a JSON value by key.  Handles simple flat and one-level-nested
# objects without requiring jq (which may not be installed in all CI
# images).
json_value() {
    python3 -c "
import json, sys
data = json.load(open('$1'))
keys = '$2'.split('.')
v = data
for k in keys:
    v = v[k]
print(v)
"
}

# Read results.
result_time=$(json_value "$RESULTS" "time_seconds")
result_northd=$(json_value "$RESULTS" "memory_mb.ovn-northd")
result_controller=$(json_value "$RESULTS" "memory_mb.ovn-controller")

# Read baseline for the given mode.
baseline_time=$(json_value "$BASELINE" "$MODE.thresholds.time_seconds")
baseline_northd=$(json_value "$BASELINE" "$MODE.thresholds.ovn-northd_mb")
baseline_controller=$(json_value "$BASELINE" "$MODE.thresholds.ovn-controller_mb")
tolerance=$(json_value "$BASELINE" "$MODE.tolerance_pct")

GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'

# Check whether a result exceeds the threshold.
check_metric() {
    local name="$1"
    local result="$2"
    local threshold="$3"

    limit=$(python3 -c "
t = float('$threshold')
tol = float('$tolerance')
print(t * (1 + tol / 100))
")

    exceeded=$(python3 -c "
print('yes' if float('$result') > float('$limit') else 'no')
")

    if [ "$exceeded" = "yes" ]; then
        echo "FAIL"
    else
        echo "PASS"
    fi
}

colorize() {
    if [ "$1" = "PASS" ]; then
        printf "${GREEN}PASS${NC}"
    else
        printf "${RED}FAIL${NC}"
    fi
}

status_time=$(check_metric "time" "$result_time" "$baseline_time")
status_northd=$(check_metric "northd memory" "$result_northd" "$baseline_northd")
status_controller=$(check_metric "controller memory" \
    "$result_controller" "$baseline_controller")

# Print results table.
printf "\n%-25s %10s %10s %10s %s\n" \
    "Metric" "Result" "Baseline" "Tolerance" "Status"
printf "%-25s %10s %10s %10s %s\n" \
    "-------------------------" "----------" "----------" "----------" "------"
printf "%-25s %10s %10s %9s%% " \
    "Time (seconds)" "$result_time" "$baseline_time" "$tolerance"
colorize "$status_time"; echo
printf "%-25s %10s %10s %9s%% " \
    "ovn-northd (MB)" "$result_northd" "$baseline_northd" "$tolerance"
colorize "$status_northd"; echo
printf "%-25s %10s %10s %9s%% " \
    "ovn-controller (MB)" "$result_controller" "$baseline_controller" "$tolerance"
colorize "$status_controller"; echo
echo ""

# Write Markdown summary for GitHub Actions.
if [ -n "$GITHUB_STEP_SUMMARY" ]; then
    {
        echo "## Performance Benchmark: $MODE"
        echo ""
        echo "| Metric | Result | Baseline | Tolerance | Status |"
        echo "|--------|--------|----------|-----------|--------|"
        md_status() { [ "$1" = "PASS" ] && echo ":white_check_mark: PASS" || echo ":x: FAIL"; }
        echo "| Time (s) | $result_time | $baseline_time" \
             "| ${tolerance}% | $(md_status "$status_time") |"
        echo "| ovn-northd (MB) | $result_northd | $baseline_northd" \
             "| ${tolerance}% | $(md_status "$status_northd") |"
        echo "| ovn-controller (MB) | $result_controller |" \
             "$baseline_controller | ${tolerance}% | $(md_status "$status_controller") |"
    } >> "$GITHUB_STEP_SUMMARY"
fi

# Exit non-zero if any metric failed.
if [ "$status_time" = "FAIL" ] || [ "$status_northd" = "FAIL" ] || \
   [ "$status_controller" = "FAIL" ]; then
    printf "${RED}REGRESSION DETECTED: one or more metrics exceeded the baseline.${NC}\n"
    exit 1
fi

printf "${GREEN}All metrics within acceptable range.${NC}\n"
