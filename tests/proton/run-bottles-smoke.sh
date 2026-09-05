#!/bin/sh
set -eu

api=
workload=
prefix=
runner_bin=
log_dir=
timeout_seconds=30

while [ "$#" -gt 0 ]; do
    case "$1" in
        --api) api=$2; shift 2 ;;
        --workload) workload=$2; shift 2 ;;
        --prefix) prefix=$2; shift 2 ;;
        --runner-bin) runner_bin=$2; shift 2 ;;
        --log-dir) log_dir=$2; shift 2 ;;
        --timeout) timeout_seconds=$2; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 64 ;;
    esac
done

case "$api" in dx11|dx12) ;; *) echo "--api must be dx11 or dx12" >&2; exit 64;; esac
[ -f "$workload" ] || { echo "workload not found: $workload" >&2; exit 66; }
[ -d "$prefix" ] || { echo "prefix not found: $prefix" >&2; exit 66; }
[ -x "$prefix/standalone" ] || { echo "Bottles standalone wrapper not found" >&2; exit 66; }
[ -x "$runner_bin/wine" ] && [ -x "$runner_bin/wineserver" ] || {
    echo "Bottles Wine runner is incomplete: $runner_bin" >&2
    exit 66
}
mkdir -p "$log_dir"
: >"$log_dir/translator.log"

layer_path=${VK_LAYER_PATH:-}
layers=${VK_INSTANCE_LAYERS:-}
ns_root=${NEUROSHADE_ROOT:-}
ns_profile=${NEUROSHADE_PROFILE:-}
ns_log=${NEUROSHADE_LOG:-$log_dir/neuroshade.log}

set +e
"$(dirname "$0")/run-contained-wine.sh" --prefix "$prefix" \
    --timeout "$timeout_seconds" -- "$prefix/standalone" /bin/sh -c '
        export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.json
        export DRI_PRIME="1002:7590!"
        export VK_LAYER_PATH="$5"
        export VK_INSTANCE_LAYERS="$6"
        export NEUROSHADE_ROOT="$7"
        export NEUROSHADE_PROFILE="$8"
        export NEUROSHADE_LOG="$9"
        export NEUROSHADE_ENABLED=1
        if [ "$4" = dx11 ]; then
            export DXVK_FILTER_DEVICE_NAME="RX 9060 XT"
            export DXVK_LOG_LEVEL=info
            export DXVK_LOG_PATH="$3"
        else
            export VKD3D_FILTER_DEVICE_NAME="RX 9060 XT"
            export VKD3D_DEBUG=info
            export VKD3D_LOG_FILE="$3/translator.log"
        fi
        "$1/wine" start /wait /unix "$2"
        result=$?
        "$1/wineserver" -k
        "$1/wineserver" -w
        exit "$result"
    ' sh "$runner_bin" "$workload" "$log_dir" "$api" "$layer_path" "$layers" \
        "$ns_root" "$ns_profile" "$ns_log"
result=$?
set -e

if [ "$api" = dx11 ]; then
    for log in "$log_dir"/*_dxgi.log "$log_dir"/*_d3d11.log; do
        [ -r "$log" ] && cat "$log"
    done
else
    [ ! -r "$log_dir/translator.log" ] || cat "$log_dir/translator.log"
fi
[ ! -r "$ns_log" ] || cat "$ns_log"
exit "$result"
