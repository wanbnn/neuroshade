# NeuroShade

NeuroShade is an AMD-only Vulkan post-processing and neural-rendering runtime for Linux and Proton. The implementation follows [SPEC.md](SPEC.md); milestones M0 through M9 are complete.

## Requirements

- Linux x86_64
- CMake 3.25+ and Make (Ninja may also be used with a manual configure)
- a C++20 compiler
- GTK4 development files for the native desktop frontend (Zenity is used as a fallback)
- Vulkan loader, headers, and a Vulkan 1.2-capable driver
- ROCm/HIP 7.x and its Clang compiler are required when `NS_BUILD_HIP_INTEROP=ON`
- MIGraphX is optional at configure time and required for the M5 runtime target; use `NS_MIGRAPHX_ROOT` for a non-system installation
- `.pth` import additionally requires a separate Python environment containing PyTorch, ONNX, NumPy, and ONNX Runtime; set `NEUROSHADE_PYTHON=/path/to/python` when it is not the default `python3`

## Build and test

```bash
cmake --preset debug
cmake --build --preset debug -j
ctest --preset debug
```

For an optimized build:

```bash
cmake --preset release
cmake --build --preset release -j
ctest --preset release
```

## User-local installation

Build the portable archive reproducibly, extract it, and install without root:

```bash
cmake --preset release -DNS_WARNINGS_AS_ERRORS=ON
cmake --build --preset release -j
cmake --build build/release --target neuroshade-portable
tar -xzf build/release/packages/NeuroShade-0.1.0-linux-x86_64.tar.gz
cd NeuroShade-0.1.0-linux-x86_64
./install.sh
~/.local/bin/neuroshade doctor --deep
```

`install.sh` verifies `SHA256SUMS` before copying the payload. It installs the
layer, tools, plugins, models, desktop entry, importer, documentation, and
qualification scripts below `~/.local`; it does not require root. Remove the
installed files while preserving profiles/logs with:

```bash
~/.local/share/neuroshade/packaging/uninstall.sh
```

Pass `--purge-user-data` only when profiles, caches, and logs should also be
deleted.

## Product tools

The CLI and the GTK4 desktop frontend remain outside the rendering process.
When GTK4 development files are unavailable at build time, a Zenity-based
fallback is installed instead:

```bash
neuroshade doctor
neuroshade frontend
neuroshade game add /path/to/game.exe ~/.config/neuroshade/my-profile.json
neuroshade profile list
neuroshade profile show /path/to/game.exe
neuroshade plugin list
neuroshade model list
neuroshade model import model.pth --adapter adapter.py
neuroshade verify
neuroshade cache clear
neuroshade diagnostic export diagnostic.tar.gz
```

Use `neuroshade launch <command...>` or the Steam launch option
`neuroshade-run %command%`. Registered profiles are recovered automatically by
exact executable identity. Session logs are written under
`$XDG_STATE_HOME/neuroshade` (normally `~/.local/state/neuroshade`).

Run the M9 reliability gates sequentially (the safe default) with:

```bash
sh ~/.local/share/neuroshade/qualification/run-m9-soaks.sh \
  ~/.local ~/.local/state/neuroshade/qualification 1800
```

Soaks share a GPU qualification lock, while Proton qualification requires it
exclusively. This prevents games, shader compilation, and native/HIP stress
loops from being launched together. Parallel soaks require the explicit
`NEUROSHADE_SOAK_PARALLEL=1` opt-in and are not recommended on a desktop GPU.

HIP/native plugins are trusted code and require `--trust-unsafe` during manual
installation. User packages and bundled plugin assets have SHA-256 inventories;
`neuroshade verify` reports modifications. See [COMPATIBILITY.md](COMPATIBILITY.md)
before using the layer with anti-cheat protected software.

The deterministic Vulkan test can also be run directly:

```bash
./build/debug/bin/ns-testbed --frames 4
```

By default it prefers a discrete AMD GPU. Select a device by name substring when needed:

```bash
NEUROSHADE_VULKAN_DEVICE='RX 9060 XT' ./build/debug/bin/ns-testbed
```

Run any Vulkan application through the explicit layer without installing it:

```bash
NEUROSHADE_ROOT="$PWD/build/debug" ./build/debug/bin/neuroshade-run <command> [arguments...]
```

Only the child process receives `VK_LAYER_NEUROSHADE`; launching the command normally disables NeuroShade without reinstalling anything.

The Layer processes intercepted swapchain presentation for shader profiles. On
neural-ready builds it also executes the fixed 64×64 bundled spatial and
temporal reference models through MIGraphX using an explicitly reported,
preallocated `HOST-STAGING-FALLBACK` presentation adapter. Incompatible model
shapes or missing artifacts fail safely to pass-through. Press Home to toggle
the Vulkan overlay; X11/XWayland uses a checked XCB global hotkey.

Exercise the M2 shader FrameGraph in the canonical testbed:

```bash
NEUROSHADE_TEST_EFFECTS=sharpen,color_adjust ./build/debug/bin/ns-testbed --frames 4
NEUROSHADE_PROFILE=tests/data/m2-profile.json ./build/debug/bin/ns-testbed --frames 4
```

Shader sources are compiled to SPIR-V before execution. Plugin manifests and pipeline profiles are parsed and validated before GPU resources are created.

Exercise the M4 Vulkan/HIP startup self-test and color pass:

```bash
NEUROSHADE_VULKAN_DEVICE='RX 9060 XT' ./build/debug/bin/ns-interop-test
NEUROSHADE_FORCE_HOST_STAGING=1 ./build/debug/bin/ns-interop-test
```

The first command uses opaque-fd external memory and keeps frame data GPU-resident. The second forces the pinned host-staging compatibility path and reports its measured test cost. Both paths use resources preallocated before the color pass. Set `NS_BUILD_HIP_INTEROP=OFF` on machines without ROCm; the hosted Vulkan CI does this explicitly.

Exercise the bundled M5 spatial model through native C++ MIGraphX:

```bash
./build/debug/bin/ns-neural-test \
  ./build/debug/share/neuroshade/models/spatial_gain.nsmodel
```

The model is compiled for the active GPU, warmed twice, and cached below `~/.cache/neuroshade/models/<model-hash>/<gfx>/<migraphx-version>/`. Tensor allocations persist for the runtime lifetime. Python is used only by build tooling to emit the bundled ONNX file and is never launched by `ns-neural-test` or the inference runtime.

Exercise the M8 true 2x temporal-SR model:

```bash
./build/debug/bin/ns-temporal-sr-test \
  ./build/debug/share/neuroshade/models/temporal_sr_2x.nsmodel
```

The qualification harness checks the compiled 32×32→64×64 tensor dimensions,
every output value for two temporal frames, motion propagation, and the
model-declared spatial fallback when motion is unavailable. The corresponding
schema-v2 profile is `src/profile/examples/temporal_sr_2x.json`.

The M0 testbed renders exact integer RGBA frames into an offscreen Vulkan image, reads the test result back only for verification, checks every pixel, and emits a sequence checksum. This readback is test-only and is not the production rendering path described by DEC-003/DEC-004.

## Status

- M0 repository, deterministic testbed, logging, build, and CI: complete
- M1 explicit Vulkan layer, pass-through, swapchain tracking, and launcher: complete
- M2 semantic FrameGraph, SPIR-V shader pipeline, manifests, and profiles: complete
- M3 resource tracking, candidate analysis, previews, bindings, and fingerprints: complete
- M4 Vulkan/HIP buffer interop, canonical ABI, self-test, fallback, and HIP kernel: complete
- M5 `.nsmodel`, native MIGraphX spatial inference, persistent tensor plan, warm-up/cache, and failure fallback: complete
- M6 temporal history ring, invalidation, profiling, and temporal inference: complete
- M7 isolated `.pth` importer, adapters, verification, and preflight: complete
- M8 motion/low-resolution binding path and true 2x temporal super-resolution: complete
- M9 present processing, Vulkan overlay/Home, product tooling, safe Proton DX11/DX12 qualification, and reproducible packaging: complete
