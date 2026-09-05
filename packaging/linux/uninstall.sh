#!/bin/sh
set -eu

prefix=${HOME:?HOME is required}/.local
purge_user_data=no
while [ "$#" -gt 0 ]; do
    case "$1" in
        --prefix) [ "$#" -ge 2 ] || { echo "uninstall.sh: --prefix needs a path" >&2; exit 64; }; prefix=$2; shift 2;;
        --purge-user-data) purge_user_data=yes; shift;;
        *) echo "usage: uninstall.sh [--prefix PATH] [--purge-user-data]" >&2; exit 64;;
    esac
done

case "$prefix" in /|""|*'/../'*|*/..) echo "uninstall.sh: unsafe prefix: $prefix" >&2; exit 64;; esac
manifest="$prefix/share/neuroshade/install-manifest.txt"
[ -r "$manifest" ] || { echo "uninstall.sh: install manifest not found: $manifest" >&2; exit 66; }

while IFS= read -r relative; do
    case "$relative" in ""|/*|../*|*'/../'*|*/..) echo "uninstall.sh: unsafe manifest entry" >&2; exit 65;; esac
    rm -f -- "$prefix/$relative"
done < "$manifest"
rm -f -- "$manifest"

for directory in \
    "$prefix/share/applications" "$prefix/share/vulkan/explicit_layer.d" \
    "$prefix/share/neuroshade/profiles/examples" "$prefix/share/neuroshade/profiles" \
    "$prefix/share/neuroshade/models" "$prefix/share/neuroshade/plugins" \
    "$prefix/share/neuroshade" "$prefix/lib/neuroshade" "$prefix/lib" \
    "$prefix/bin" "$prefix/share"; do
    rmdir --ignore-fail-on-non-empty "$directory" 2>/dev/null || true
done

if [ "$purge_user_data" = yes ]; then
    config="${XDG_CONFIG_HOME:-$HOME/.config}/neuroshade"
    cache="${XDG_CACHE_HOME:-$HOME/.cache}/neuroshade"
    state="${XDG_STATE_HOME:-$HOME/.local/state}/neuroshade"
    for directory in "$config" "$cache" "$state"; do
        case "$directory" in /|""|"$HOME") echo "uninstall.sh: unsafe data directory" >&2; exit 65;; esac
        rm -rf -- "$directory"
    done
fi

printf 'NeuroShade removed from %s\n' "$prefix"
[ "$purge_user_data" = yes ] || printf 'User profiles, caches, and logs were preserved.\n'
