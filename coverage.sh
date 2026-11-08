#!/usr/bin/env bash
# Collect and report code coverage for the SC compiler.
#
# Usage: ./coverage.sh [MIN_COVERAGE]
#
# Builds with --coverage, runs ./test.sh against the instrumented binary,
# then uses llvm-cov gcov to aggregate per-file line coverage and enforce
# a minimum coverage gate (default 80%).
set -Eeuo pipefail
cd "$(dirname "$0")"

MIN_COV="${1:-80}"
BUILD_DIR="$PWD/.sc-coverage-build"

# 1. Instrumented build (binary lands in the build dir so .gcda/.gcno stay
#    next to each other).
CXX="${CXX:-clang++}"
CXXFLAGS="-Wall -Wextra --coverage" ./build.sh

# 2. Run the full test suite against the instrumented binary.
SC_SKIP_BUILD=1 SC_BIN="$BUILD_DIR/sc" ./test.sh

# 3. Aggregate per-file coverage with llvm-cov gcov.
#    gcov prints "Lines executed:X% of N" for the source file first, then for
#    every header it includes. We only want the SC source files themselves,
#    so run gcov once per source and take the first matching line.
SOURCES=(main lexer parser semantic codegen)
total_executed=0
total_lines=0
declare -a FILE_REPORTS=()
for src in "${SOURCES[@]}"; do
    srcfile="$src.cpp"
    # gcov writes .gcov annotation files next to the source, so redirect its
    # stdout to a temp file and grep that. This also avoids SIGPIPE from
    # `grep -m1` closing the pipe early under `set -o pipefail`.
    gcov_out=$(mktemp)
    llvm-cov gcov -o "$BUILD_DIR" "$srcfile" >"$gcov_out" 2>/dev/null
    line=$(grep -m1 'Lines executed' "$gcov_out" || true)
    rm -f "$gcov_out"
    if [[ "$line" =~ Lines\ executed:([0-9.]+)%\ of\ ([0-9]+) ]]; then
        pct="${BASH_REMATCH[1]}"
        n="${BASH_REMATCH[2]}"
        executed=$(awk "BEGIN { printf \"%d\", $pct * $n / 100 }")
        total_executed=$((total_executed + executed))
        total_lines=$((total_lines + n))
        FILE_REPORTS+=("$srcfile: $line")
    fi
done

if [[ "$total_lines" -eq 0 ]]; then
    echo "ERROR: no coverage data collected" >&2
    exit 1
fi

pct=$(awk "BEGIN { printf \"%.2f\", $total_executed * 100.0 / $total_lines }")
echo "Total line coverage: $pct% ($total_executed / $total_lines lines)"

# 4. Emit a human-readable per-file report.
echo "=== Per-file coverage ==="
for r in "${FILE_REPORTS[@]}"; do
    echo "  $r"
done

# 5. Enforce the gate. Strip the trailing '%' so awk does a numeric
#    comparison (string comparison would mis-order e.g. 9 < 80).
pct_num="${pct%\%}"
if awk -v total="$pct_num" -v min="$MIN_COV" 'BEGIN { exit !(total+0 < min+0) }'; then
    echo "ERROR: coverage $pct% is below the $MIN_COV% minimum" >&2
    exit 1
fi
echo "Coverage gate passed: $pct% >= $MIN_COV%"