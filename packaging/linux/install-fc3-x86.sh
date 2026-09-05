#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
prefix=${NEUROSHADE_INSTALL_PREFIX:-"$HOME/.local"}
PKG_CONFIG_LIBDIR=/usr/lib32/pkgconfig:/usr/share/pkgconfig cmake -S "$repo" -B "$repo/build/fc3-x86" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS=-m32 -DCMAKE_SHARED_LINKER_FLAGS=-m32 \
    -DCMAKE_EXE_LINKER_FLAGS=-m32 -DVulkan_LIBRARY=/usr/lib32/libvulkan.so \
    -DNS_BUILD_NEURAL=OFF -DNS_BUILD_HIP_INTEROP=OFF -DNS_BUILD_TESTBED=OFF -DBUILD_TESTING=OFF
cmake --build "$repo/build/fc3-x86" --target neuroshade_layer ns-package-inspect -j 6
runtime="$prefix/share/neuroshade/farcry3/runtime"
assets="$runtime/share/neuroshade"
mkdir -p "$assets/shaders" "$prefix/lib/neuroshade/fc3-x86" "$prefix/share/neuroshade/farcry3/layers"
for name in plugins models; do
    if [ ! -e "$assets/$name" ]; then ln -s "$prefix/share/neuroshade/$name" "$assets/$name"; fi
done
cp "$prefix/share/neuroshade/shaders/"*.spv "$assets/shaders/"
glslc --target-env=vulkan1.2 -O "$repo/plugins/bundled/overlay/overlay.comp" -o "$assets/shaders/overlay.spv.new"
mv "$assets/shaders/overlay.spv.new" "$assets/shaders/overlay.spv"
lib="$prefix/lib/neuroshade/fc3-x86/libVkLayer_neuroshade.so"
cp "$repo/build/fc3-x86/lib/neuroshade/libVkLayer_neuroshade.so" "$lib.new"
mv "$lib.new" "$lib"
python3 - "$prefix" <<'PY'
import json,sys
from pathlib import Path
p=Path(sys.argv[1]);manifest=json.loads((p/'share/vulkan/explicit_layer.d/VkLayer_neuroshade.json').read_text())
manifest['layer']['library_path']=str(p/'lib/neuroshade/fc3-x86/libVkLayer_neuroshade.so')
(p/'share/neuroshade/farcry3/layers/VkLayer_neuroshade.json').write_text(json.dumps(manifest,indent=2)+'\n')
PY
printf '%s\n' 'x86 Vulkan layer + host64 interface installed. Restart the game to load it.'
