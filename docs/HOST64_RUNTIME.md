# Spatial reconstruction and functional overlay

The Vulkan layer now renders text and interactive controls instead of the fixed
decorative rectangles. Home toggles the overlay; F1–F7 select pages, arrows and
Enter navigate controls, and pointer capture enables mouse buttons where available.
Effects can be enabled, reordered, adjusted, applied and saved. Presets can be
created and loaded. Models can be selected and validated. The overlay includes
presentation FPS, frame history, Vulkan timestamps for NeuroShade's GPU passes,
separate host inference timing, and per-GPU Linux sensors where exposed.

## Engines

- The existing native MIGraphX spatial and temporal runtimes remain in the tree.
- The new host64 service executes spatial PyTorch models through Spandrel. This
  provides architecture detection for supported image networks such as ESRGAN,
  RealESRGAN variants and SwinIR. Actual compatibility depends on the checkpoint;
  unsupported architectures and failed exports are rejected.
- ONNX Runtime consumes spatial ONNX `.nsmodel` packages. The default provider is
  explicitly CPUExecutionProvider; CPU execution is never reported as GPU work.
- A private Unix socket bridges the 32-bit Vulkan layer to the native 64-bit
  service. One spatial neural pass can precede ordinary shader passes and the UI.
  The bridge preserves alpha, handles RGBA/BGRA, uses overlapping tiles and keeps
  the original input if frame inference fails. Reapply the model after fixing a
  disconnected service. Temporal/motion-guided models use the pre-existing native
  path and are not supported by the host64 spatial service.

## Installation on the tested Arch/ROCm host

Install a GPU-capable system PyTorch first. Then, from the repository:

```sh
./packaging/linux/install-host64.sh
./packaging/linux/install-fc3-x86.sh
~/.local/bin/neuroshade-runtime start
~/.local/bin/neuroshade-runtime status
```

The host installer creates a venv with system packages and installs Spandrel,
ONNX, ONNX Runtime, NumPy and Pillow. The x86 installer requires multilib C++,
Vulkan and XCB development libraries under `/usr/lib32`. It keeps the standard
64-bit installation intact and installs a matching private overlay shader for FC3.

For Bottles, enable inheritance of `VK_LAYER_PATH`, `VK_INSTANCE_LAYERS`,
`NEUROSHADE_ROOT`, `NEUROSHADE_PROFILE`, `NEUROSHADE_LOG`, `NEUROSHADE_ENABLED` and
`NEUROSHADE_OVERLAY_VISIBLE` in the bottle's `Inherited_Environment_Variables`
when `Limit_System_Environment` is enabled. Flatpak `--env` alone is insufficient.
`examples/launch-farcry3.sh` documents the tested launch path; set
`NEUROSHADE_GAME_ROOT` to the game directory. The layer in this Soda runner must
be 32-bit even though the host64 inference service uses 64-bit PyTorch.

## Import and select models

```sh
~/.local/bin/neuroshade-runtime import /path/model.pth \
  ~/.local/share/neuroshade/models/my-model.nsmodel
```

`.safetensors` checkpoints are also accepted. Import exports ONNX, checks the
model, compares ONNX output against PyTorch, and installs only after validation.
The package includes the real weights, ONNX graph, signature, metadata, checksums
and preview. Model weights are not included in this Git repository.

In the overlay: **F3 Models → select model → Usar modelo → Aplicar**. Initial
warmup may take several seconds. Save the profile to persist the selection.
Use **Sem modelo → Aplicar** to return to the shader-only pipeline.

## Configuration and measurements

The service reads `~/.config/neuroshade/runtime.json` when loading a model:

```json
{
  "backend": "auto",
  "device": "cuda:0",
  "precision": "auto",
  "tile": 192,
  "overlap": 24,
  "input_mode": "reconstruct",
  "onnx_provider": "CPUExecutionProvider",
  "cpu_threads": 4
}
```

PyTorch uses the `cuda` device API for ROCm as well. `reconstruct` reduces the
presented image by the model's scale before reconstruction to display size.
This is post-processing of the presented frame, not integration with the game's
internal render resolution. `native` processes full-resolution input and reduces
the model's output back to presentation size; it costs considerably more.

```sh
~/.local/bin/neuroshade-runtime benchmark /path/model.nsmodel --width 1920 --height 1080
~/.local/bin/neuroshade-runtime process /path/model.nsmodel input.png output.png
```

On the tested RX 9060 XT, `4x_DIV2K-Lite_1M` was detected as ESRGAN, exported with
maximum absolute ONNX/PyTorch difference 0.00000453, and ran in FP16 through ROCm.
Warm standalone 1080p inference measured about 163–167 ms; the FC3 integration
reported about 172 ms. This model is functional but not suitable for high-FPS
1080p gameplay in this implementation. First-time kernel warmup was much slower.
The socket and GPU/CPU transfers add overhead beyond model inference.

Logs: `~/.local/state/neuroshade/host64.log` and `fc3-runtime.log`.
The host is a background service started on demand, not a system-wide daemon.

## Validation

- 32-bit layer compiled and executed in Far Cry 3/DXVK at 1920×1080 with ESRGAN,
  two shader passes and visible UI; the user confirmed successful operation.
- PyTorch/ONNX numerical export comparison and actual ROCm reconstruction.
- ONNX CPU inference on an existing `.nsmodel` reference package.
- Tests for IPC, tensor contracts, channel order, alpha preservation, tiling,
  checksums, UI edits, presets, profile persistence and existing pipeline state.

```sh
PYTHONPATH=runtime/python ~/.local/share/neuroshade/runtime-venv/bin/python \
  -m unittest discover -s tests/runtime -v
ctest --test-dir build/ui-check \
  -R '^(runtime-ui|unit.overlay_state|profile.schema_v2_roundtrip|unit.pipeline_plan)$' \
  --output-on-failure
```
