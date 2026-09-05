# NeuroShade — Implementation Goals

This document tracks implementation against `SPEC.md`. The specification remains the source of truth; a checked item here means there is implementation and local evidence, not merely scaffolding.

Status legend: `[x]` complete and verified, `[~]` implemented but awaiting an external gate, `[ ]` pending.

## Environment baseline

- [x] Linux x86_64 development host.
- [x] Vulkan 1.4.357 loader available.
- [x] AMD Radeon RX 9060 XT detected as RADV GFX1200.
- [x] ROCm/HIP 7.2 detects `gfx1200`.
- [x] Khronos validation library located in the Steam runtime and exposed through a test-only build manifest.

## M0 — Repository and deterministic testbed

Deliveries:

- [x] Repository directory structure from SPEC section 5.
- [x] CMake C++20 build with Debug and Release presets.
- [x] Thread-safe logging core with severity filtering.
- [x] Deterministic Vulkan offscreen testbed.
- [x] Debug/Release GitHub Actions workflow.
- [x] CTest unit-test harness.
- [x] User-facing build and test documentation.

Acceptance evidence:

- [x] `ns-testbed` renders and verifies deterministic integer RGBA animation.
- [x] Debug build and both tests pass locally.
- [x] Release build with warnings-as-errors and both tests pass locally.
- [x] Repeated 8-frame runs produce checksum `0x523f44c4ebf8a325` on GFX1200.
- [x] Staged install contains the testbed, common library, and public logging header.

Status: complete locally. The hosted CI workflow will gain independent evidence on its first repository run.

## M1 — Production-safe Vulkan Layer

Deliveries:

- [x] Explicit `VK_LAYER_NEUROSHADE` manifest.
- [x] Loader/layer interface negotiation.
- [x] Per-instance and per-device downstream dispatch state.
- [x] Swapchain lifecycle, image acquisition, image enumeration, and present tracking.
- [x] Per-game `neuroshade-run` opt-in wrapper preserving arguments and exit status.
- [x] Native pass-through path without frame mutation.
- [x] CTest pass-through integration test.

Acceptance evidence:

- [x] Native Vulkan testbed runs through the layer on GFX1200.
- [x] Layer output checksum `0x3d23155e380d6325` matches the native output.
- [x] Deterministic testbed passes with Khronos validation and no `Validation Error`/`VUID` output.
- [x] CI automatically adds the validation test when `VK_LAYER_KHRONOS_validation` is installed.
- [x] Layer can be disabled without reinstalling and native checksum remains unchanged.
- [x] Wrapper propagates the child exit status and preserves arguments.
- [x] `vkcube` swapchain smoke test presents 12 frames on RX 9060 XT and exits successfully.
- [x] Validated `vkcube` swapchain smoke test presents 100 frames without validation errors.
- [x] Twenty-five consecutive layer load/create/destroy runs pass.

Status: complete and locally verified on RX 9060 XT/GFX1200.

## M2 — Shader FrameGraph

Deliveries:

- [x] Declarative FrameGraph and compiler with deterministic cache keys.
- [x] Semantic resource abstraction and versioned frame representation.
- [x] Canonical testbed final-color capture.
- [x] Preallocated SPIR-V compute pass runtime.
- [x] Vulkan buffer-to-image output composite with recapture verification.
- [x] Versioned plugin manifest parser.
- [x] Bundled copy, sharpen, and color-adjustment shaders.

Acceptance:

- [x] Bundled sharpen changes the checksum from `0x3d23155e380d6325` to `0xec30b79b3c1fe325`.
- [x] Effect state changes during a running multi-frame test.
- [x] Two shader effects can be ordered, with distinct checksums for each order.
- [x] Versioned profile saves, reloads, and restores enabled state and pass order.
- [x] Capture, compute barriers, composite, and recapture pass Khronos Validation without VUIDs.
- [x] GPU resources and execution plan are allocated outside per-frame execution.

Status: complete and locally verified on RX 9060 XT/GFX1200 using the canonical Vulkan testbed.

## M3 — Resource analyzer

Deliveries:

- [x] Stable non-reused runtime IDs and tracked Vulkan image/memory/view lifetimes.
- [x] Depth candidate scoring from format, usage, extent, writes, and attachment behavior.
- [x] Scene-color and low-resolution color candidate scoring.
- [x] Ranked candidate lists and all SPEC preview visualization modes.
- [x] Manual semantic resource binding, including intentional wrong selections and correction.
- [x] Versioned persistent fingerprints and confidence-based restart rebind.
- [x] Layer observation of image creation/destruction, memory binding, views, dynamic rendering, swapchains, and presents.

Acceptance:

- [x] Real Vulkan D32 testbed fixture is identified as the highest-confidence depth candidate.
- [x] Intentional wrong depth candidate can be bound.
- [x] Correct depth candidate can replace the wrong binding.
- [x] Binding survives serialization/restart and rebinds a changed runtime ID at confidence `1.0`.
- [x] Destroyed Vulkan handles and dependent views are removed from the tracker.
- [x] Instrumented layer passes a 100-frame `vkcube` smoke test under Khronos Validation.

Status: complete and locally verified on RX 9060 XT/GFX1200.

## M4 — HIP interop

Documentation policy: use official AMD ROCm/HIP documentation, not Context7.

Deliveries:

- [x] Triple-buffered exportable Vulkan pool with `InputColor[3]`, `Depth[3]`, `Motion[3]`, `OutputColor[3]`, and preallocated scratch.
- [x] Versioned canonical ABI and Vulkan optimal-image↔packed-FP16-buffer conversion.
- [x] Cached HIP opaque-fd external-memory imports targeting `gfx1200`.
- [x] Startup Vulkan↔HIP hash/pattern self-test cached per Vulkan device/driver, HIP architecture, and ROCm runtime.
- [x] Preallocated pinned host-staging compatibility fallback with visible mode/cost reporting.
- [x] Bundled HIP color-channel kernel and host-coordinated Vulkan/HIP synchronization.

Acceptance:

- [x] Vulkan pattern is read by HIP with hash `0xa998ec3e0c388fa5`.
- [x] HIP result is consumed through a Vulkan image and validates as hash `0xadf131247d4eeb25`.
- [x] HIP color effect changes the canonical testbed image checksum.
- [x] Preferred steady-state path reports `steady_state_cpu_frame_readback=no`; full-frame readback exists only in the qualification self-test so Vulkan can validate HIP output.
- [x] `NEUROSHADE_FORCE_HOST_STAGING=1` selects and validates `Interop: HOST-STAGING FALLBACK`.

Qualification evidence:

- [x] Debug and Release builds complete with warnings treated as errors.
- [x] All 12 CTest cases pass on RX 9060 XT/GFX1200, including both interop modes and automatic fallback after an injected self-test failure.
- [x] Zero-copy interop passes Khronos Validation without VUIDs.
- [x] Staged install contains and successfully runs `ns-interop-test`.

Status: complete and locally verified on RX 9060 XT/GFX1200 with ROCm/HIP 7.2.

## M5 — Neural spatial runtime

Documentation policy: use official AMD MIGraphX documentation, not Context7.

Deliveries:

- [x] Versioned `.nsmodel` directory package, strict manifest/signature/metadata loader, shape declaration, and content hash.
- [x] Native C++ MIGraphX GPU integration with offload copies disabled.
- [x] Persistent HIP input/output tensor plan created from the compiled MIGraphX parameter shapes.
- [x] GPU/architecture/runtime-specific compiled-model cache and two-pass warm-up before activation.
- [x] Bundled deterministic 1× spatial ONNX model and installed preview/metadata assets.

Acceptance:

- [x] Bundled model changes checksum from `0x867ddf4dacfb5060` to `0xffba0c1c4fb3e0b0` on GFX1200.
- [x] Installed inference executable and model run without Python; Python is used only to generate the ONNX build asset.
- [x] Model compiles and completes two warm-up evaluations before reporting active.
- [x] Injected execution failure disables the neural pass and preserves the input through the GPU copy fallback.

Qualification evidence:

- [x] MIGraphX 2.15 parses and compiles the bundled ONNX model for the GPU target.
- [x] Debug and Release builds complete with warnings treated as errors.
- [x] All 13 CTest cases pass on RX 9060 XT/GFX1200, including real MIGraphX inference/cache/fallback.
- [x] Staged install contains the complete `.nsmodel` package and executes it successfully.

Status: complete and locally verified on RX 9060 XT/GFX1200 with MIGraphX 2.15 and ROCm/HIP 7.2.

## M6 — Temporal runtime

Deliveries:

- [x] GPU-resident 4-slot temporal history ring (`src/neural/temporal/ring_buffer.{hpp,cpp}`), HIP-event-backed per-pass profiler (`src/telemetry/profiler.{hpp,cpp}`).
- [x] History invalidation on resize, discontinuity, and binding changes (`TemporalRuntime::reset_history` and `invalidate_history`).
- [x] Temporal model semantics — `History.Color.*` inputs classified by the manifest loader and bound to ring slots at every `step()`; `Output.Color` recorded into the next slot each frame.
- [x] Per-pass GPU profiler with HIP-event timing and EMA smoothing (SPEC §23, UI-004, §49), pass IDs `neural.temporal.*` emitted from `ns-temporal-test`.
- [x] Bundled deterministic `temporal_blend.nsmodel` model (single-Add ONNX graph with `history=1`, `first_frame=spatial`) built from `tools/nsmodel/generate_temporal_model.py`.

Acceptance:

- [x] Model consumes frame N and history N-1 — `step()` rebinds the history slot to the previous frame's recorded output and the second-step checksum `0xa120eaad9207f63a` differs from the first-step checksum `0xaf4240c164425093` on GFX1200.
- [x] Explicit history reset works — `reset_history()` and `invalidate_history("swapchain_recreate")` both replay the first-frame checksum `0xaf4240c164425093`.
- [x] Swapchain recreation never consumes stale history — `invalidate_history("swapchain_recreate")` zeroes the ring buffer and clears `history_valid_`, exercised in `ns-temporal-test`.
- [x] Resolution changes do not crash — `invalidate_history("resolution_change")` routes through the same reset path; no per-frame allocation, profiler reports stable GPU-event timings (first frame 0.108 ms, second frame 0.049 ms).

Qualification evidence:

- [x] All 12 CTest cases pass on RX 9060 XT/GFX1200 with MIGraphX 2.15 and ROCm/HIP 7.2, including the new `neural.temporal_runtime` test against the bundled `temporal_blend.nsmodel`.
- [x] Failures (`NEUROSHADE_TEST_TEMPORAL_FAILURE=1`) disable the pass and preserve the input through the GPU copy fallback; `pass_disabled=yes` and `last_error="injected temporal execution failure"` recorded.
- [x] Compiled-model cache is reused on second instance (`compiled_cache=hit`); cache key combines the bundle content hash, gfx architecture, and MIGraphX runtime version.
- [x] Profiler exposes `gpu_profiler=hip_events` and per-pass moving averages for `neural.temporal.first_frame_eval`, `neural.temporal.second_frame_eval`, `neural.temporal.history_record`.
- [x] Debug + Release builds complete with warnings as errors.
- [x] Staged install contains `ns-temporal-test`, `libneuroshade_temporal.a`, and the bundled `temporal_blend.nsmodel`; the staged binary reproduces the same checksums when run with `LD_LIBRARY_PATH` pointing at the ROCm and MIGraphX library directories.

Status: complete and locally verified on RX 9060 XT/GFX1200 with MIGraphX 2.15 and ROCm/HIP 7.2.

## M7 — `.pth` model importer

Deliveries:

- [x] Isolated Python importer process (`tools/nsmodel/importer.py` entry + `importer/python/neuroshade_importer/` library). The C++ runtime never touches `.pth`. The importer runs as its own `python3` process invoked by the user and by CTest.
- [x] Architecture adapter API (`ArchitectureAdapter` protocol + `adapter_from_path` sandbox loader using `importlib.util.spec_from_file_location`). Two bundled adapters ship in `importer/adapters/`: `spatial_gain.py` and `unsupported_op.py` (the latter for IMP-005 trip-wire testing).
- [x] Weights-only loading by default (`torch.load(..., weights_only=True)` in `core.run_import`); explicit `--unsafe-pickle` flag flips the default after a stderr warning.
- [x] ONNX export goes through each adapter's `export_onnx` method (sentinel adapters can hand-roll ONNX bytes; reference adapters call `torch.onnx.export` with opset 18).
- [x] Source/export numerical verification (`verifier.verify` using `onnxruntime` as the runtime backend; `verification=tolerance=1e-05 max_abs_diff=…` recorded in `metadata.json` and on stdout).
- [x] `.nsmodel` packaging with FNV-1a `content_hash` byte-identical to the C++ loader's `stable_hash` (`package.cpp:238-249`); 5-file order matches; the runtime cache key still resolves to the same per-architecture slot.

Acceptance:

- [x] Known reference `.pth` imports successfully — `tools/nsmodel/build_reference_model.py` writes `reference_spatial_gain.pth`; the importer produces `reference_spatial_gain.nsmodel/` and `ns-neural-test` round-trips the bytes with `input_hash=0x867ddf4dacfb5060` / `neural_hash=0xffba0c1c4fb3e0b0` matching the bundled spatial model (proves the C++ loader accepts the importer's package).
- [x] Source and exported outputs agree within tolerance — `verification=tolerance=1e-05 max_abs_diff=0.000e+00 backend=onnxruntime 1.29.0` recorded on every successful `import`; the `importer.roundtrip` CTest reads the same keys off stdout.
- [x] Unknown architecture fails with a clear error — `importer.bad_architecture` corrupts the `.pth`'s state_dict and asserts `status=fail reason=adapter_error`.
- [x] Unsupported MIGraphX operations fail before game launch — `importer.unsupported_op` drives the bundled adapter whose ONNX uses the `Optional` op (opset 17). Preflight runs first and reports `preflight_error: structural preflight flagged unsupported ops: ['Optional']`, short-circuiting verifier + writer before any `.nsmodel` materialises.

Qualification evidence:

- [x] 16/16 CTest cases pass on RX 9060 XT/GFX1200 with MIGraphX 2.15 and ROCm/HIP 7.2: the original 12 (M0–M6) plus `importer.adapter_protocol`, `importer.roundtrip`, `importer.bad_architecture`, `importer.unsupported_op`.
- [x] Reference model on disk: `build/m7/share/neuroshade/importer/reference_spatial_gain.nsmodel/` (5 files, 51-byte preview, content_hash round-trips byte-identical with the C++ loader).
- [x] Staged install (`cmake --install`) now includes `usr/local/share/neuroshade/models/{spatial_gain,temporal_blend,reference_spatial_gain}.nsmodel/`.
- [x] All exception paths emit `status=fail reason=<…>` on stdout and a longer explanation on stderr, with deterministic exit codes (`2=adapter`, `3=verification`, `4=preflight`, `5=package`, `1=internal`).
- [x] `cmake --build` with `-DNS_WARNINGS_AS_ERRORS=ON` succeeds; no new warnings.
- [x] Staged run smoke-tested: `LD_LIBRARY_PATH=… ns-neural-test …/reference_spatial_gain.nsmodel` exits 0 and reports the same `bundled_model_changed_output=yes` as the bundled build-time model.

Status: complete and locally verified on RX 9060 XT/GFX1200 with MIGraphX 2.15 and ROCm/HIP 7.2.

## M8 — Motion and true temporal super-resolution

Documentation policy: use official AMD MIGraphX/ROCm/HIP documentation, not Context7.

Deliveries:

- [x] Motion-vector candidate scoring for signed/float two-component formats, screen-relative extent, writable usage, and repeated writes.
- [x] Motion-vector XY preview mapping signed vectors to RG visualization.
- [x] Manual `Motion.Screen` binding and fingerprint persistence/rebind across changed runtime IDs.
- [x] Low-resolution `Color.LowRes` scene-source candidate, manual binding, and fingerprint persistence/rebind.
- [x] Schema-v2 example 2× temporal-SR profile with model and semantic resource bindings.
- [x] Native C++ schema-v2 parse/save/reload test; the incomplete Python placeholder was removed.
- [x] Bundled three-input `temporal_sr_2x.nsmodel` using ONNX `Resize` nearest-neighbor from 32×32 to 64×64 plus high-resolution temporal history.
- [x] Model-level `missing_motion` policy and runtime spatial fallback that clears history and motion without disabling the pass.

Acceptance:

- [x] Testbed creates and ranks a half-resolution scene-color source.
- [x] MIGraphX compiles a 32×32 input to a 64×64 output (`output_bytes=65536`) on GFX1200; the qualification harness verifies every FP32 output element.
- [x] Two distinct motion fields produce distinct temporal output checksums (`0xe85604adf8c16555` and `0xcb1664645d6c63e5`).
- [x] Missing motion selects `missing_motion=spatial`, zeroes temporal inputs/history, reconstructs the spatial result, and leaves the pass active.

Qualification evidence:

- [x] Debug and Release builds complete with warnings treated as errors.
- [x] 20/20 CTest cases pass in Debug and Release on RX 9060 XT/GFX1200, including `profile.schema_v2_roundtrip` and `neural.temporal_sr_2x`.
- [x] Temporal-SR qualification passes ten consecutive executions after explicit HIP history synchronization.
- [x] Staged install contains `ns-temporal-sr-test`, the complete five-file `temporal_sr_2x.nsmodel`, and `profiles/examples/temporal_sr_2x.json`.
- [x] The staged binary executes without Python and reproduces true 2× output, motion propagation, and spatial fallback on GFX1200.

Status: complete and locally verified on RX 9060 XT/GFX1200 with MIGraphX 2.15 and ROCm/HIP 7.2.

## M9 — Productization

Deliveries:

- [x] Native GTK4 desktop frontend outside the rendering hot path, backed by the same file/tool interface as the CLI and with a display-free noninteractive self-test; a Zenity implementation remains as the build fallback when GTK4 is unavailable.
- [x] User-local installer and manifest-driven uninstaller; normal uninstall preserves profiles, caches, and logs unless `--purge-user-data` is explicit.
- [x] `neuroshade` CLI commands for doctor, launch, games/profiles, plugins, models, isolated `.pth` import, cache clearing, verification, logs, and frontend launch.
- [x] Plugin and model managers with atomic user installs, strict IDs, model package completeness checks, and symlink/path-traversal rejection.
- [x] Installed native package inspector reuses the production C++ manifest and `.nsmodel` loaders before accepting shader plugins or models.
- [x] Explicit `--trust-unsafe` gate for native/HIP plugins.
- [x] SHA-256 inventories for bundled plugin assets, portable payload, and user-installed plugins/models, including modified-package detection.
- [x] `neuroshade doctor` capability diagnostics, compatibility class, JSON output, and optional real interop/neural `--deep` self-test.
- [x] Reproducible portable tarball builder with fixed metadata, complete payload checksum verification, installer, uninstaller, documentation, and archive checksum.
- [x] Reproducible `.pth` reference import with sanitized ONNX provenance: Debug and Release packages are byte-identical and contain no checkout/user path.
- [x] Compatibility and anti-cheat documentation with qualification dates and explicit unqualified paths (`COMPATIBILITY.md`).
- [x] Per-session wrapper log under `$XDG_STATE_HOME/neuroshade`, with the Vulkan layer appending to the same file.
- [x] Profile-aware `neuroshade-run` lookup and default-deny warning policy for common anti-cheat bootstrap/service executables.
- [x] The target process Layer loads and validates the selected profile and logs an explicit safe pass-through fallback for invalid profiles; wrapper diagnostics run before Layer activation.
- [x] Layer startup resolves enabled shader/model artifacts and compiles the profile-selected FrameGraph before GPU allocation; missing or invalid packages retain `PASS_THROUGH` and the log distinguishes a prepared plan from connected execution.
- [x] Proton DX11/DX12 qualification harness records pinned component versions, exit status, translator/GPU evidence, and logs without confusing synthetic harness validation with real qualification.
- [x] Overlay state/controller for Home visibility, all seven specified tabs, production runtime modes, pass enable/reorder/strength/model/default controls, resource rows, profiler averages, VRAM, compatibility, and accessible text output.
- [x] Parameterized 30-minute native/temporal soak harness plus short CTest contract.
- [x] Connect the profile-selected FrameGraph to intercepted presentation: SPIR-V shaders run directly; bundled spatial and temporal MIGraphX models run through a visible, preallocated HIP host-staging adapter; top resource candidates and HIP-event timing feed the overlay state. Unsupported shapes retain pass-through.
- [x] In-game Vulkan compute overlay renderer and checked XCB Home-key input adapter, with a deterministic X11 event gate and ten repeated toggle runs.
- [x] Real Bottles/ProtoSoda DX11/DXVK qualification on a pinned deterministic 90-present workload, with shader processing active on the RX 9060 XT.
- [x] Real Bottles/ProtoSoda DX12/VKD3D-Proton qualification on a pinned deterministic 90-present workload, with shader processing active on the RX 9060 XT.
- [x] Execute and record the full 30-minute native and temporal soak gates sequentially: 7,308 native iterations and 8,310 temporal/MIGraphX iterations, both 1,800 seconds with zero failures.

Acceptance:

- [x] Complete SPEC section 43 user journey is covered by the isolated install/profile/manager/restart test plus real intercepted shader, Home overlay, spatial neural, temporal history, resource-candidate, and profiler gates.
- [x] Thirty-minute testbed and temporal-model soak tests pass, with independently verified atomic result records and zero failures.
- [x] Release gates in SPEC section 47 pass, including validation-clean presentation, explicit fallback reporting, sequential full soaks, and real contained Proton cases.
- [x] Every v1 Definition of Done item in SPEC section 46 has implementation and local qualification evidence.

Qualification evidence so far:

- [x] `product.tools` installs into an isolated user prefix, runs doctor, manages/reloads profiles, installs and verifies shader/model packages, detects tampering, enforces native-plugin trust, clears cache, exercises the desktop frontend, launches with automatic profile recovery, rejects a synthetic anti-cheat executable, and uninstalls while preserving user state.
- [x] `product.portable_package` builds the tarball twice with the same SHA-256, extracts it, verifies payload checksums through `install.sh`, and uninstalls it.
- [x] `product.session_log` confirms wrapper and Vulkan layer messages share the user-visible session log.
- [x] `proton.harness_contract` validates orchestration and result recording using synthetic launchers; it is explicitly not DX11/DX12 qualification.
- [x] `reliability.soak_harness` completes short native and temporal loops with zero failures.
- [x] Debug and Release each pass all 31 CTest cases on RX 9060 XT/GFX1200 with warnings as errors in Release.
- [x] The Release portable archive checksum verifies, contains no checkout/user paths, and its only runtime search paths are the product ROCm locations under `/opt/rocm`.
- [x] On RX 9060 XT/GFX1200, `neuroshade doctor --deep` reports `neural-ready`, interop self-test `pass`, and neural self-test `pass`.
- [x] Valve Proton 11.0-2, Bottles 67.2, DXVK 3.0.2, VKD3D-Proton 3.0.1, and two unprotected Unreal workloads were found in the secondary Steam library; `/usr/bin/proton` remains the unrelated Triton profiler command.
- [x] A combined-soak/Bottles incident is documented in `INCIDENTS.md`; qualification locks, fail-fast Proton execution, and sequential full soaks now prevent the unsafe concurrency by default.
- [x] Repeated the interrupted soak gates sequentially; full result records passed. Earlier partial logs remain excluded from acceptance evidence.
- [x] `vulkan.present_processing` executes two ordered shaders, renders the Vulkan overlay, toggles it with Home, and passes Khronos Validation; the deterministic toggle gate also passes ten consecutive runs.
- [x] `vulkan.neural_present` executes real MIGraphX inference in intercepted presentation, changes output pixels, reports HIP-event average cost, and validates both copy fallback and incompatible-shape pass-through.
- [x] `vulkan.temporal_present` executes two or more presented frames, records valid temporal history, changes output pixels, and passes Khronos Validation.
- [x] Final Proton record `build/m9-proton/final-qualification.json` reports `qualified=yes`, both exits zero, DXVK/VKD3D/Layer/GPU evidence yes, and zero residual Wine/game processes.
- [x] llvm-mingw 20260826 produces byte-identical DX fixtures; hashes are `72aef406…16b1e` (DX11) and `3e696ba5…949b` (DX12).
- [x] Release portable archive is reproducible with SHA-256 `1d914fe02b8a7cef877c8fcdb60afa83e7e8846d08183d1b3bc1dade8d4f2637` and contains the contained runner plus relocatable DX fixture sources/build script.

Status: complete. M9 productization and all locally executable v1 release gates are verified on RX 9060 XT/GFX1200.
