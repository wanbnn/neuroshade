#!/bin/sh
# Install the host64 spatial engines and the matching x86 Vulkan UI.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
prefix=${NEUROSHADE_INSTALL_PREFIX:-"$HOME/.local"}
venv="$prefix/share/neuroshade/runtime-venv"
python3 -m venv --system-site-packages "$venv"
"$venv/bin/python" -m pip install -r "$repo/runtime/requirements.txt"
mkdir -p "$prefix/share/neuroshade/runtime/python" "$prefix/bin"
cp -R "$repo/runtime/python/neuroshade_runtime" "$prefix/share/neuroshade/runtime/python/"
cat > "$prefix/bin/neuroshade-runtime" <<'WRAPPER'
#!/bin/sh
set -eu
prefix=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
export PYTHONPATH="$prefix/share/neuroshade/runtime/python${PYTHONPATH:+:$PYTHONPATH}"
exec "$prefix/share/neuroshade/runtime-venv/bin/python" -m neuroshade_runtime "$@"
WRAPPER
chmod +x "$prefix/bin/neuroshade-runtime"
printf '%s\n' "Installed: $prefix/bin/neuroshade-runtime" "The system PyTorch must support your GPU (ROCm/CUDA) for accelerated inference."
