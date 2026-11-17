#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
# SPDX-License-Identifier: MIT
#
# Runs the test executables under Valgrind (memcheck by default) and summarises the results.
#
# Each GoogleTest executable runs once under Valgrind (much faster than one Valgrind process
# per CTest case). Logs go to <build>/valgrind/<executable>.{valgrind,test}.log. The summary
# separates Valgrind errors (memory errors, definite/indirect leaks, data races) from test
# failures: tests run 20-50x slower under Valgrind, so timing-based waits may fail on their own.
#
# Use a build without sanitizers (Valgrind cannot run ASan binaries), e.g. the "debug" preset.
# System-library noise is suppressed by tests/valgrind.supp; our own code must stay clean.

set -euo pipefail

readonly SOURCE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

usage() {
    cat <<EOF
Usage: $(basename "$0") [options] [executable ...]

Runs the test executables (default: all except ds_gl_tests) under Valgrind.
Executables are given by name, e.g. ds_unit_tests ds_gui_tests.

Options:
  -b, --build-dir DIR    Build tree to test (default: build/debug). Must not use sanitizers.
  -t, --tool TOOL        Valgrind tool: memcheck (default), helgrind or drd.
  -f, --filter PATTERN   GoogleTest filter for every executable (--gtest_filter).
  -j, --jobs N           Executables run in parallel (default: 2).
  -o, --output DIR       Log directory (default: <build-dir>/valgrind).
      --timeout MINUTES  Time limit per executable (default: 120).
      --gl               Also run ds_gl_tests (native OpenGL 4.5; slow, needs a display).
      --fast             memcheck without --track-origins (about twice as fast, less detail).
      --gen-suppressions Write suppression blocks for every error into the logs, to extend
                         tests/valgrind.supp with genuine system-library noise.
      --list             List the test executables and exit.
  -h, --help             Show this help.

Examples:
  $(basename "$0")                                  # Whole suite, memcheck.
  $(basename "$0") -f 'MissionRunner.*' ds_unit_tests
  $(basename "$0") --tool helgrind ds_mission_flight_tests
  $(basename "$0") --gl -f 'Viewport.*' ds_gl_tests

Exit status: 0 if clean, 1 on Valgrind errors or test failures, 2 on usage errors.
EOF
}

die() {
    echo "run-valgrind: $*" >&2
    exit 2
}

build_dir="${SOURCE_DIR}/build/debug"
tool="memcheck"
filter=""
jobs=2
output_dir=""
timeout_min=120
with_gl=false
fast=false
gen_suppressions=false
list_only=false
selected=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        -b | --build-dir) build_dir="${2:?missing value for $1}"; shift 2 ;;
        -t | --tool) tool="${2:?missing value for $1}"; shift 2 ;;
        -f | --filter) filter="${2:?missing value for $1}"; shift 2 ;;
        -j | --jobs) jobs="${2:?missing value for $1}"; shift 2 ;;
        -o | --output) output_dir="${2:?missing value for $1}"; shift 2 ;;
        --timeout) timeout_min="${2:?missing value for $1}"; shift 2 ;;
        --gl) with_gl=true; shift ;;
        --fast) fast=true; shift ;;
        --gen-suppressions) gen_suppressions=true; shift ;;
        --list) list_only=true; shift ;;
        -h | --help) usage; exit 0 ;;
        -*) die "unknown option '$1' (see --help)" ;;
        *) selected+=("$1"); shift ;;
    esac
done

case "${tool}" in
    memcheck | helgrind | drd) ;;
    *) die "unsupported tool '${tool}' (memcheck, helgrind or drd)" ;;
esac
[[ "${jobs}" =~ ^[1-9][0-9]*$ ]] || die "--jobs needs a positive number"
[[ "${timeout_min}" =~ ^[1-9][0-9]*$ ]] || die "--timeout needs a positive number of minutes"
command -v valgrind >/dev/null || die "valgrind is not installed (apt install valgrind)"

[[ -d "${build_dir}" ]] || die "build directory '${build_dir}' does not exist"
build_dir="$(cd "${build_dir}" && pwd)"
cache="${build_dir}/CMakeCache.txt"
[[ -f "${cache}" ]] || die "'${build_dir}' is not a CMake build tree"
if grep -q '^DS_ENABLE_SANITIZERS:BOOL=ON' "${cache}"; then
    die "'${build_dir}' is a sanitizer (ASan) build; Valgrind needs a plain build, e.g. build/debug"
fi
output_dir="${output_dir:-${build_dir}/valgrind}"
suppressions="${SOURCE_DIR}/tests/valgrind.supp"

# --- Test executables -----------------------------------------------------------------------
# The GoogleTest executables CTest knows about (stale binaries of removed targets are ignored).
declare -A available=()
command -v ctest >/dev/null || die "ctest is not installed"
while IFS= read -r exe; do
    [[ -f "${exe}" && -x "${exe}" ]] && available["$(basename "${exe}")"]="${exe}"
done < <(cd "${build_dir}" && ctest -N -V 2>/dev/null |
    sed -n 's|.*Test command: \([^ ]*/tests/[^ /]*/ds_[A-Za-z0-9_]*\)\( .*\)\{0,1\}$|\1|p' | sort -u)
[[ ${#available[@]} -gt 0 ]] || die "no test executables in '${build_dir}' (build the tests first)"

if ${list_only}; then
    printf '%s\n' "${!available[@]}" | sort
    exit 0
fi

if [[ ${#selected[@]} -eq 0 ]]; then
    while IFS= read -r name; do
        if [[ "${name}" == "ds_gl_tests" ]] && ! ${with_gl}; then
            continue
        fi
        selected+=("${name}")
    done < <(printf '%s\n' "${!available[@]}" | sort)
fi
for name in "${selected[@]}"; do
    [[ -n "${available[${name}]:-}" ]] || die "unknown test executable '${name}' (see --list)"
done

# --- Valgrind command line ------------------------------------------------------------------
vg_args=(
    "--tool=${tool}"
    --error-exitcode=99
    --num-callers=40
    --fair-sched=yes          # Fair thread scheduling: the simulation loop runs in a thread.
    --child-silent-after-fork=yes
    "--suppressions=${suppressions}"
)
case "${tool}" in
    memcheck)
        vg_args+=(
            --leak-check=full
            --show-leak-kinds=definite,indirect
            --errors-for-leak-kinds=definite,indirect
        )
        ${fast} || vg_args+=(--track-origins=yes)
        ;;
    helgrind | drd) ;;
esac
${gen_suppressions} && vg_args+=(--gen-suppressions=all)

mkdir -p "${output_dir}"

# QRegularExpression's PCRE2 JIT generates code that memcheck reports as uninitialised reads
# with no symbols (false positives); the interpreter gives the same results.
export QT_ENABLE_REGEXP_JIT=0

# Runs one executable; writes its result line to <output>/<name>.result.
run_one() {
    local name="$1"
    local exe="${available[${name}]}"
    local vg_log="${output_dir}/${name}.valgrind.log"
    local test_log="${output_dir}/${name}.test.log"
    local test_args=()
    [[ -n "${filter}" ]] && test_args+=("--gtest_filter=${filter}")

    local start status=0
    start=$(date +%s)
    # The tests use their own temporary directories; QT_QPA_PLATFORM is chosen by the tests.
    (cd "$(dirname "${exe}")" &&
        timeout --kill-after=30s "${timeout_min}m" \
            valgrind "${vg_args[@]}" "--log-file=${vg_log}" "${exe}" "${test_args[@]}") \
        >"${test_log}" 2>&1 || status=$?
    local seconds=$(($(date +%s) - start))

    local errors failed passed
    errors=$(sed -n 's/.*ERROR SUMMARY: \([0-9]*\) errors.*/\1/p' "${vg_log}" 2>/dev/null | tail -1)
    passed=$(sed -n 's/^\[  PASSED  \] \([0-9]*\) test.*/\1/p' "${test_log}" | tail -1)
    # Failed tests as listed in gtest's final summary (after "[==========] N tests ... ran.").
    failed=$(sed -n '/^\[==========\] .* ran\./,$p' "${test_log}" |
        grep -c '^\[  FAILED  \] [A-Za-z_][A-Za-z0-9_/]*\.' || true)
    local verdict="ok"
    if [[ ${status} -eq 124 || ${status} -eq 137 ]]; then
        verdict="timeout"
    elif [[ -z "${errors}" ]]; then
        verdict="crashed"
    elif [[ "${errors}" -gt 0 ]]; then
        verdict="valgrind-errors"
    elif [[ "${failed}" -gt 0 || ${status} -ne 0 ]]; then
        verdict="test-failures"
    fi
    printf '%s %s %s %s %s %s\n' "${name}" "${verdict}" "${errors:--}" "${passed:-0}" \
        "${failed:-0}" "${seconds}" >"${output_dir}/${name}.result"
    printf '  %-32s %-16s (%ss)\n' "${name}" "${verdict}" "${seconds}"
}

echo "Valgrind ${tool} on ${#selected[@]} executable(s) from ${build_dir}"
[[ -n "${filter}" ]] && echo "GoogleTest filter: ${filter}"
echo "Logs: ${output_dir}"
echo

rm -f "${output_dir}"/*.result
running=0
for name in "${selected[@]}"; do
    run_one "${name}" &
    running=$((running + 1))
    if [[ ${running} -ge ${jobs} ]]; then
        wait -n || true
        running=$((running - 1))
    fi
done
wait || true

# --- Summary --------------------------------------------------------------------------------
echo
printf '%-32s %-16s %8s %8s %8s %8s\n' "Executable" "Result" "VG errs" "Passed" "Failed" "Seconds"
bad=0
for name in "${selected[@]}"; do
    result="${output_dir}/${name}.result"
    if [[ ! -f "${result}" ]]; then
        printf '%-32s %-16s\n' "${name}" "no-result"
        bad=1
        continue
    fi
    read -r _ verdict errors passed failed seconds <"${result}"
    printf '%-32s %-16s %8s %8s %8s %8s\n' "${name}" "${verdict}" "${errors}" "${passed}" \
        "${failed}" "${seconds}"
    [[ "${verdict}" == "ok" ]] || bad=1
done

if [[ ${bad} -ne 0 ]]; then
    echo
    echo "Details:"
    for name in "${selected[@]}"; do
        result="${output_dir}/${name}.result"
        [[ -f "${result}" ]] || continue
        read -r _ verdict _ <"${result}"
        case "${verdict}" in
            valgrind-errors) echo "  ${name}: see ${output_dir}/${name}.valgrind.log" ;;
            test-failures | timeout | crashed)
                echo "  ${name}: see ${output_dir}/${name}.test.log and ${name}.valgrind.log" ;;
        esac
    done
    exit 1
fi
echo
echo "No Valgrind errors and no test failures."
