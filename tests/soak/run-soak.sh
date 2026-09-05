#!/bin/sh
set -eu

mode=
build_dir=
duration=1800
iterations=0
output=soak-result.txt
while [ "$#" -gt 0 ]; do
    case "$1" in
        --mode) mode=$2; shift 2;;
        --build-dir) build_dir=$2; shift 2;;
        --duration) duration=$2; shift 2;;
        --iterations) iterations=$2; shift 2;;
        --output) output=$2; shift 2;;
        *) echo "usage: run-soak.sh --mode testbed|temporal --build-dir DIR [--duration SEC|--iterations N] [--output FILE]" >&2; exit 64;;
    esac
done
[ "$mode" = testbed ] || [ "$mode" = temporal ] || { echo "invalid soak mode" >&2; exit 64; }
[ -d "$build_dir" ] || { echo "build directory not found" >&2; exit 66; }

# Soaks may run together, but must never overlap an interactive Proton/game
# qualification. The latter can otherwise multiply Vulkan devices, shader
# compiler threads, and GPU memory pressure on the desktop GPU.
command -v flock >/dev/null 2>&1 || { echo "flock utility is required" >&2; exit 69; }
lock_path=${NEUROSHADE_QUALIFICATION_LOCK:-${XDG_RUNTIME_DIR:-${TMPDIR:-/tmp}}/neuroshade-gpu-qualification-$(id -u).lock}
exec 9>"$lock_path"
flock -s -n 9 || { echo "another exclusive GPU qualification is active: $lock_path" >&2; exit 75; }

case "$mode" in
    testbed) executable="$build_dir/bin/ns-testbed";;
    temporal) executable="$build_dir/bin/ns-temporal-sr-test";;
esac
[ -x "$executable" ] || { echo "soak executable not found: $executable" >&2; exit 66; }

start=$(date +%s)
deadline=$((start + duration))
count=0
failures=0
log="$output.log"
: > "$log"
while :; do
    now=$(date +%s)
    if [ "$iterations" -gt 0 ]; then
        [ "$count" -lt "$iterations" ] || break
    else
        [ "$now" -lt "$deadline" ] || { [ "$count" -gt 0 ] && break; }
    fi
    if [ "$mode" = testbed ]; then
        NEUROSHADE_TEST_EFFECTS=sharpen,color_adjust NEUROSHADE_TEST_TOGGLE_FRAME=60 \
            "$executable" --frames 120 >>"$log" 2>&1 || failures=$((failures + 1))
    else
        XDG_CACHE_HOME="${XDG_CACHE_HOME:-${HOME:?HOME is required}/.cache}/neuroshade-soak" \
            "$executable" "$build_dir/share/neuroshade/models/temporal_sr_2x.nsmodel" \
            >>"$log" 2>&1 || failures=$((failures + 1))
    fi
    count=$((count + 1))
    [ "$failures" -eq 0 ] || break
done
elapsed=$(($(date +%s) - start))
temporary="$output.tmp.$$"
printf 'schema_version=1\nmode=%s\niterations=%s\nelapsed_seconds=%s\nfailures=%s\n' \
    "$mode" "$count" "$elapsed" "$failures" > "$temporary"
mv "$temporary" "$output"
cat "$output"
[ "$failures" -eq 0 ]
