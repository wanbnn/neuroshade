#!/bin/sh
# Public bootstrap: download a versioned, checksum-pinned native DLSSNR bundle.
set -eu
prefix=${HOME:?HOME is required}/.local
while [ "$#" -gt 0 ]; do
    case "$1" in
        --prefix) [ "$#" -ge 2 ] || exit 64; prefix=$2; shift 2;;
        --help|-h) echo 'usage: install.sh [--prefix PATH]'; exit 0;;
        *) echo 'usage: install.sh [--prefix PATH]' >&2; exit 64;;
    esac
done
[ "$(uname -s)" = Linux ] && [ "$(uname -m)" = x86_64 ] || {
    echo 'This bundle requires Linux x86-64. Native Windows is not supported yet.' >&2; exit 69;
}
for program in curl tar sha256sum; do
    command -v "$program" >/dev/null 2>&1 || { echo "Install the required tool: $program" >&2; exit 69; }
done
work=$(mktemp -d "${TMPDIR:-/tmp}/neuroshade-download.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
archive=NeuroShade-dlssnr-0.1.2-gfx1200-linux-x86_64.tar.gz
url="https://github.com/wanbnn/neuroshade/releases/download/dlssnr-v0.1.2/$archive"
printf '%s\n' 'Downloading NeuroShade + DLSSNR (gfx1200, 1920x1080; ROCm/HIP 7 required)...'
curl --fail --location --retry 3 --proto '=https' --proto-redir '=https' "$url" -o "$work/$archive"
printf '%s  %s\n' 'b34cfff6c97f693c1315b1cf497edad667ce12a8c0e0126529e487b7d3e250d7' "$archive" > "$work/SHA256SUMS"
(cd "$work" && sha256sum -c SHA256SUMS)
tar -xzf "$work/$archive" -C "$work"
sh "$work/NeuroShade-dlssnr-0.1.2-gfx1200/install.sh" --prefix "$prefix"
