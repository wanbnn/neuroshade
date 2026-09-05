#!/bin/sh
set -eu

[ "$#" -eq 2 ] || { echo "usage: build-portable.sh <cmake-build-dir> <output-dir>" >&2; exit 64; }
build_dir=$(CDPATH= cd -- "$1" && pwd)
output_dir=$2
source_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
version=$(sed -n 's/^project(NeuroShade VERSION \([^ ]*\).*/\1/p' "$source_dir/CMakeLists.txt")
[ -n "$version" ] || { echo "unable to determine version" >&2; exit 65; }
mkdir -p "$output_dir"
output_dir=$(CDPATH= cd -- "$output_dir" && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/neuroshade-package.XXXXXX")
trap 'rm -rf -- "$work"' EXIT HUP INT TERM
package="$work/NeuroShade-$version-linux-x86_64"
mkdir -p "$package/payload"
cmake --install "$build_dir" --prefix "$package/payload"
cp "$source_dir/packaging/linux/install.sh" "$package/install.sh"
cp "$source_dir/packaging/linux/uninstall.sh" "$package/uninstall.sh"
cp "$source_dir/README.md" "$source_dir/COMPATIBILITY.md" "$source_dir/LICENSE" "$package/"
chmod 755 "$package/install.sh" "$package/uninstall.sh"
(cd "$package/payload" && find . -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum) > "$package/SHA256SUMS"
epoch=${SOURCE_DATE_EPOCH:-0}
archive="$output_dir/NeuroShade-$version-linux-x86_64.tar.gz"
tar --sort=name --mtime="@$epoch" --owner=0 --group=0 --numeric-owner \
    -C "$work" -czf "$archive" "NeuroShade-$version-linux-x86_64"
sha256sum "$archive" > "$archive.sha256"
printf '%s\n' "$archive"
