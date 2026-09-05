#!/bin/sh
set -eu

wrapper=${NEUROSHADE_RUN:-neuroshade-run}
output=proton-qualification.json
dx9=
dx11=
dx12=
timeout_seconds=${NEUROSHADE_PROTON_TIMEOUT:-120}
expected_gpu=${NEUROSHADE_EXPECTED_GPU:-}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --dx9-launcher) dx9=$2; shift 2;;
        --dx11-launcher) dx11=$2; shift 2;;
        --dx12-launcher) dx12=$2; shift 2;;
        --output) output=$2; shift 2;;
        *) echo "usage: qualify-proton.sh --dx9-launcher FILE --dx11-launcher FILE --dx12-launcher FILE [--output FILE]" >&2; exit 64;;
    esac
done

[ -x "$dx9" ] || { echo "DX9 launcher is not executable: $dx9" >&2; exit 66; }
[ -x "$dx11" ] || { echo "DX11 launcher is not executable: $dx11" >&2; exit 66; }
[ -x "$dx12" ] || { echo "DX12 launcher is not executable: $dx12" >&2; exit 66; }
command -v timeout >/dev/null 2>&1 || { echo "timeout utility is required" >&2; exit 69; }
command -v flock >/dev/null 2>&1 || { echo "flock utility is required" >&2; exit 69; }

# Proton/game qualification is deliberately exclusive with native/HIP soaks.
# This protects the desktop GPU from simultaneous context churn and shader
# compilation, and makes a failure attributable to one workload.
lock_path=${NEUROSHADE_QUALIFICATION_LOCK:-${XDG_RUNTIME_DIR:-${TMPDIR:-/tmp}}/neuroshade-gpu-qualification-$(id -u).lock}
exec 9>"$lock_path"
flock -x -n 9 || { echo "another GPU qualification is active: $lock_path" >&2; exit 75; }

run_case() {
    api=$1
    launcher=$2
    log=$3
    set +e
    timeout --kill-after=10s "$timeout_seconds" "$wrapper" "$launcher" >"$log" 2>&1
    result=$?
    set -e
    printf '%s' "$result"
}

output_dir=${output%/*}
[ "$output_dir" = "$output" ] || mkdir -p "$output_dir"
dx9_log="$output.dx9.log"
dx11_log="$output.dx11.log"
dx12_log="$output.dx12.log"
dx9_result=$(run_case dx9 "$dx9" "$dx9_log")
if [ "$dx9_result" -eq 0 ] || [ "${NEUROSHADE_PROTON_CONTINUE_AFTER_FAILURE:-0}" = 1 ]; then
    dx11_result=$(run_case dx11 "$dx11" "$dx11_log")
else
    dx11_result=125
    printf 'not run: DX9 qualification failed with exit %s\n' "$dx9_result" >"$dx11_log"
fi
if [ "$dx11_result" -eq 0 ] || [ "${NEUROSHADE_PROTON_CONTINUE_AFTER_FAILURE:-0}" = 1 ]; then
    dx12_result=$(run_case dx12 "$dx12" "$dx12_log")
else
    dx12_result=125
    printf 'not run: DX11 qualification failed with exit %s\n' "$dx11_result" >"$dx12_log"
fi
dx9_evidence=no
dx11_evidence=no
dx12_evidence=no
dx9_layer_evidence=no
dx11_layer_evidence=no
dx12_layer_evidence=no
grep -Eiq 'DXVK([: =]|$)' "$dx9_log" &&
    grep -Eiq 'D3D9([: =]|$)' "$dx9_log" && dx9_evidence=yes
grep -Eiq 'DXVK([: =]|$)' "$dx11_log" && dx11_evidence=yes
grep -Eiq 'VKD3D[- ]?Proton([: =]|$)' "$dx12_log" && dx12_evidence=yes
grep -Eq 'present_processing=active backend=shader' "$dx9_log" && dx9_layer_evidence=yes
grep -Eq 'present_processing=active backend=shader' "$dx11_log" && dx11_layer_evidence=yes
grep -Eq 'present_processing=active backend=shader' "$dx12_log" && dx12_layer_evidence=yes
gpu_evidence=yes
if [ -n "$expected_gpu" ]; then
    gpu_evidence=no
    grep -Fq "$expected_gpu" "$dx9_log" &&
        grep -Fq "$expected_gpu" "$dx11_log" && grep -Fq "$expected_gpu" "$dx12_log" &&
        gpu_evidence=yes
fi
proton_version=${PROTON_VERSION:-unknown}
dxvk_version=${DXVK_VERSION:-unknown}
vkd3d_version=${VKD3D_PROTON_VERSION:-unknown}
qualified=no
[ "$dx9_result" -eq 0 ] && [ "$dx11_result" -eq 0 ] && [ "$dx12_result" -eq 0 ] &&
    [ "$dx9_evidence" = yes ] && [ "$dx9_layer_evidence" = yes ] &&
    [ "$dx11_evidence" = yes ] && [ "$dx12_evidence" = yes ] &&
    [ "$dx11_layer_evidence" = yes ] && [ "$dx12_layer_evidence" = yes ] &&
    [ "$gpu_evidence" = yes ] && qualified=yes

temporary="$output.tmp.$$"
printf '{"schema_version":2,"qualified":"%s","proton":"%s","dxvk":"%s","vkd3d_proton":"%s","expected_gpu":"%s","gpu_evidence":"%s","dx11_evidence":"%s","dx12_evidence":"%s","dx11_layer_evidence":"%s","dx12_layer_evidence":"%s","dx11_exit":%s,"dx12_exit":%s,"dx11_log":"%s","dx12_log":"%s","dx9_evidence":"%s","dx9_layer_evidence":"%s","dx9_exit":%s,"dx9_log":"%s"}\n' \
    "$qualified" "$proton_version" "$dxvk_version" "$vkd3d_version" \
    "$expected_gpu" "$gpu_evidence" "$dx11_evidence" "$dx12_evidence" \
    "$dx11_layer_evidence" "$dx12_layer_evidence" \
    "$dx11_result" "$dx12_result" "$dx11_log" "$dx12_log" \
    "$dx9_evidence" "$dx9_layer_evidence" "$dx9_result" "$dx9_log" > "$temporary"
mv "$temporary" "$output"
cat "$output"
[ "$qualified" = yes ]
