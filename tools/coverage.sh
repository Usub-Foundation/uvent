#!/usr/bin/env bash
# Line coverage of the uvent library from the test suite (clang source-based
# coverage). Run through the `coverage` CMake target of a build configured with
# -DUVENT_COVERAGE=ON, or directly: tools/coverage.sh <build dir>.
#
# Outputs, all inside <build dir>/coverage/:
#   report.txt   per-file table (llvm-cov report)
#   lcov.info    lcov tracefile (llvm-cov export -format=lcov) for genhtml/Codecov
#   html/        browsable report (llvm-cov show)
#
# Env: LLVM_SUFFIX (e.g. "-18" to pick llvm-cov-18), CTEST_PARALLEL (default 3),
#      UVENT_TEST_SCALE forwarded to the tests.
set -euo pipefail
build=${1:?build dir}
suffix=${LLVM_SUFFIX:-}
profdata="llvm-profdata${suffix}"; cov="llvm-cov${suffix}"
command -v "$profdata" >/dev/null || { echo "$profdata not found (set LLVM_SUFFIX)"; exit 1; }
out="$build/coverage"; raw="$out/raw"
rm -r -f "$out"; mkdir -p "$raw"

# One profile per test binary; %m merges the forked children of the harness
# into their parent's file (see flush_coverage_profile in tests/test_common.h).
( cd "$build" && LLVM_PROFILE_FILE="$raw/%m-%p.profraw" \
    ctest -j"${CTEST_PARALLEL:-3}" --timeout 900 --output-on-failure )
"$profdata" merge -sparse "$raw"/*.profraw -o "$out/merged.profdata"

objects=()
for t in "$build"/tests/test_*; do [ -x "$t" ] && [ ! -d "$t" ] && objects+=(-object "$t"); done
ignore='(/tests/|_deps/|/usr/|/examples/)'
"$cov" report "${objects[@]}" -instr-profile="$out/merged.profdata" -ignore-filename-regex="$ignore" > "$out/report.txt"
"$cov" export "${objects[@]}" -instr-profile="$out/merged.profdata" -ignore-filename-regex="$ignore" -format=lcov > "$out/lcov.info"
"$cov" show "${objects[@]}" -instr-profile="$out/merged.profdata" -ignore-filename-regex="$ignore" \
    -format=html -output-dir="$out/html" -show-line-counts-or-regions -show-branches=count > /dev/null
tail -1 "$out/report.txt" | awk '{printf "uvent coverage: lines %s  functions %s  regions %s\n", $10, $7, $4}'
echo "report: $out/report.txt  lcov: $out/lcov.info  html: $out/html/index.html"
