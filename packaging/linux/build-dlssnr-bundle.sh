#!/bin/sh
# Package an existing native installation; no game-specific paths in the payload.
set -eu
[ "$#" -eq 3 ] || { echo 'usage: build-dlssnr-bundle.sh <installed-prefix> <x86-layer.so> <output-dir>' >&2; exit 64; }
prefix=$(CDPATH= cd -- "$1" && pwd)
x86=$2
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
mkdir -p "$3"
output=$(CDPATH= cd -- "$3" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
package="$work/NeuroShade-dlssnr-0.1.2-gfx1200"
payload="$package/payload"
mkdir -p "$payload/bin" "$payload/lib/neuroshade/x86" "$payload/libexec/neuroshade" \
    "$payload/share/neuroshade/runtime/lib" "$payload/share/neuroshade/models" \
    "$payload/share/neuroshade/layers/x86" "$payload/share/vulkan/explicit_layer.d"
cp "$prefix/lib/neuroshade/libVkLayer_neuroshade.so" "$payload/lib/neuroshade/"
cp "$x86" "$payload/lib/neuroshade/x86/libVkLayer_neuroshade.so"
cp "$prefix/libexec/neuroshade/ns-dlssnr-host" "$payload/libexec/neuroshade/"
cp "$prefix/share/neuroshade/runtime/lib/libneuroshade_dlssnr.so" "$payload/share/neuroshade/runtime/lib/"
cp -a "$prefix/share/neuroshade/models/dlssnr-1080p.nsmodel" "$payload/share/neuroshade/models/"
cp -a "$prefix/share/neuroshade/plugins" "$prefix/share/neuroshade/shaders" "$payload/share/neuroshade/"
cp "$prefix/share/vulkan/explicit_layer.d/VkLayer_neuroshade.json" "$payload/share/vulkan/explicit_layer.d/"
# Relative paths keep both layers relocatable after installation.
sed 's#../../../lib/neuroshade/libVkLayer_neuroshade.so#../../../../lib/neuroshade/x86/libVkLayer_neuroshade.so#' \
    "$payload/share/vulkan/explicit_layer.d/VkLayer_neuroshade.json" > "$payload/share/neuroshade/layers/x86/VkLayer_neuroshade.json"
sed 's/@PROJECT_VERSION@/0.1.0/g' "$repo/tools/neuroshade-run/neuroshade-run.in" > "$payload/bin/neuroshade-run"
sed 's/@PROJECT_VERSION@/0.1.0/g' "$repo/tools/neuroshade-cli/neuroshade.in" > "$payload/bin/neuroshade"
cp "$repo/tools/neuroshade-run/neuroshade-setup" "$payload/bin/"
for tool in ns-package-inspect ns-dlssnr-inspect; do
    [ ! -x "$prefix/libexec/neuroshade/$tool" ] || cp "$prefix/libexec/neuroshade/$tool" "$payload/libexec/neuroshade/"
done
[ ! -x "$prefix/bin/neuroshade-desktop" ] || cp "$prefix/bin/neuroshade-desktop" "$payload/bin/"
chmod 755 "$payload/bin/"*
cp "$repo/packaging/linux/install.sh" "$repo/packaging/linux/uninstall.sh" "$package/"
cp "$repo/README.md" "$repo/LICENSE" "$repo/COMPATIBILITY.md" "$package/"
cp -a "$repo/images" "$repo/docs" "$package/"
(cd "$payload" && find . -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum) > "$package/SHA256SUMS"
archive="$output/NeuroShade-dlssnr-0.1.2-gfx1200-linux-x86_64.tar.gz"
tar --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner -C "$work" -czf "$archive" "$(basename "$package")"
(cd "$output" && sha256sum "$(basename "$archive")") > "$archive.sha256"
printf '%s\n' "$archive"
