#!/bin/sh
set -eu

[ "$#" -ge 2 ] || { echo "usage: run-m9-soaks.sh <build-dir> <output-dir> [duration-seconds]" >&2; exit 64; }
build_dir=$1
output_dir=$2
duration=${3:-1800}
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mkdir -p "$output_dir"

# Hold a shared lock for the complete suite so an exclusive Proton run cannot
# slip into the transition between the native and temporal stages.
command -v flock >/dev/null 2>&1 || { echo "flock utility is required" >&2; exit 69; }
lock_path=${NEUROSHADE_QUALIFICATION_LOCK:-${XDG_RUNTIME_DIR:-${TMPDIR:-/tmp}}/neuroshade-gpu-qualification-$(id -u).lock}
exec 8>"$lock_path"
flock -s -n 8 || { echo "another exclusive GPU qualification is active: $lock_path" >&2; exit 75; }

if [ "${NEUROSHADE_SOAK_PARALLEL:-0}" = 1 ]; then
    /bin/sh "$script_dir/run-soak.sh" --mode testbed --build-dir "$build_dir" \
        --duration "$duration" --output "$output_dir/testbed.txt" &
    testbed_pid=$!
    /bin/sh "$script_dir/run-soak.sh" --mode temporal --build-dir "$build_dir" \
        --duration "$duration" --output "$output_dir/temporal.txt" &
    temporal_pid=$!
    set +e
    wait "$testbed_pid"
    testbed_result=$?
    wait "$temporal_pid"
    temporal_result=$?
    set -e
else
    # Sequential is the production default: shader compilation or a second
    # graphics workload must not obscure resource-pressure failures.
    set +e
    /bin/sh "$script_dir/run-soak.sh" --mode testbed --build-dir "$build_dir" \
        --duration "$duration" --output "$output_dir/testbed.txt"
    testbed_result=$?
    if [ "$testbed_result" -eq 0 ]; then
        /bin/sh "$script_dir/run-soak.sh" --mode temporal --build-dir "$build_dir" \
            --duration "$duration" --output "$output_dir/temporal.txt"
        temporal_result=$?
    else
        temporal_result=125
    fi
    set -e
fi

printf 'testbed_exit=%s\ntemporal_exit=%s\n' "$testbed_result" "$temporal_result"
[ "$testbed_result" -eq 0 ] && [ "$temporal_result" -eq 0 ]
