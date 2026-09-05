#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
prefix=${HOME:?HOME is required}/.local
payload="$script_dir/payload"

while [ "$#" -gt 0 ]; do
    case "$1" in
        --prefix) [ "$#" -ge 2 ] || { echo "install.sh: --prefix needs a path" >&2; exit 64; }; prefix=$2; shift 2;;
        --payload) [ "$#" -ge 2 ] || { echo "install.sh: --payload needs a path" >&2; exit 64; }; payload=$2; shift 2;;
        *) echo "usage: install.sh [--prefix PATH] [--payload PATH]" >&2; exit 64;;
    esac
done

case "$prefix" in /|""|*'/../'*|*/..) echo "install.sh: unsafe prefix: $prefix" >&2; exit 64;; esac
[ -d "$payload" ] || { echo "install.sh: payload not found: $payload" >&2; exit 66; }
[ -f "$payload/share/vulkan/explicit_layer.d/VkLayer_neuroshade.json" ] || {
    echo "install.sh: payload has no Vulkan layer manifest" >&2; exit 65;
}

if [ -f "$script_dir/SHA256SUMS" ]; then
    (cd "$payload" && sha256sum -c "$script_dir/SHA256SUMS")
fi

config_dir="${XDG_CONFIG_HOME:-$HOME/.config}/neuroshade"
mkdir -p "$prefix/bin" "$prefix/lib" "$prefix/share" \
    "$config_dir" \
    "${XDG_CACHE_HOME:-$HOME/.cache}/neuroshade" \
    "${XDG_STATE_HOME:-$HOME/.local/state}/neuroshade"

manifest_dir="$prefix/share/neuroshade"
mkdir -p "$manifest_dir"
manifest="$manifest_dir/install-manifest.txt"
temporary="$manifest.tmp.$$"
(cd "$payload" && find . -type f -print | LC_ALL=C sort | sed 's#^./##') > "$temporary"
cp -a "$payload/." "$prefix/"
mv "$temporary" "$manifest"
if [ ! -e "$config_dir/config.json" ]; then
    config_temporary="$config_dir/.config.json.tmp.$$"
    printf '%s\n' '{"schema_version":1,"frame_budget_ms":4.0,"adaptive_bypass":false}' > "$config_temporary"
    mv "$config_temporary" "$config_dir/config.json"
fi

printf 'NeuroShade installed without root in %s\n' "$prefix"
printf 'Run: %s/bin/neuroshade doctor\n' "$prefix"
