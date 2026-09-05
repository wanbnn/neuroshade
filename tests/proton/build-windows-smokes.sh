#!/bin/sh
set -eu

root=${LLVM_MINGW_ROOT:?set LLVM_MINGW_ROOT to the extracted llvm-mingw directory}
output=${1:-build/proton-workloads}
compiler="$root/bin/x86_64-w64-mingw32-clang++"
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
source_dir=${NEUROSHADE_WINDOWS_SMOKE_SOURCE:-"$script_dir/windows"}
[ -x "$compiler" ] || { echo "compiler not found: $compiler" >&2; exit 66; }
[ -r "$source_dir/dx9_smoke.cpp" ] && [ -r "$source_dir/dx11_smoke.cpp" ] && [ -r "$source_dir/dx12_smoke.cpp" ] || {
    echo "Windows smoke sources not found: $source_dir" >&2
    exit 66
}
mkdir -p "$output"

common='-std=c++20 -O2 -Wall -Wextra -Werror -municode -mwindows -Wl,--no-insert-timestamp'
# shellcheck disable=SC2086
"$compiler" $common "$source_dir/dx9_smoke.cpp" \
    -o "$output/neuroshade-dx9-smoke.exe" -ld3d9 -luser32
# shellcheck disable=SC2086
"$compiler" $common "$source_dir/dx11_smoke.cpp" \
    -o "$output/neuroshade-dx11-smoke.exe" -ld3d11 -ldxgi -luser32
# shellcheck disable=SC2086
"$compiler" $common "$source_dir/dx12_smoke.cpp" \
    -o "$output/neuroshade-dx12-smoke.exe" -ld3d12 -ldxgi -luser32

sha256sum "$output/neuroshade-dx9-smoke.exe" "$output/neuroshade-dx11-smoke.exe" "$output/neuroshade-dx12-smoke.exe"
