# NeuroShade Compatibility

This file records tested paths. “Infrastructure available” is not equivalent to
qualification: a path is marked qualified only after the stated workload has
actually completed.

## Compatibility classes

| Class | Meaning |
|---|---|
| `neural-ready` | Vulkan, ROCm/HIP, MIGraphX, layer manifest, and layer library are available. |
| `hip-fallback` | Vulkan and HIP are available, but native neural inference is unavailable. |
| `shader-only` | Vulkan shader processing is available; HIP/neural effects are unavailable. |
| `unsupported` | The minimum Vulkan product path was not detected. |

Run `neuroshade doctor` to display the local class. This is a capability hint,
not a promise that every game is compatible.

## Qualified systems

| Date | GPU/driver | Runtime | Workload | Result |
|---|---|---|---|---|
| 2026-09-04 | RX 9060 XT, RADV GFX1200 | Vulkan native | `ns-testbed`, layer pass-through, shader pipeline, validation | Qualified |
| 2026-09-04 | RX 9060 XT, ROCm/HIP 7.2 | HIP/Vulkan | zero-copy and forced host-staging qualification | Qualified |
| 2026-09-04 | RX 9060 XT, MIGraphX 2.15 | Neural | spatial, temporal, and true 2× temporal SR models | Qualified |
| 2026-09-04 | RX 9060 XT + Renoir iGPU, Bottles 67.2/ProtoSoda 11.0-1, DXVK 3.0.2 | Unreal Engine DX11 (`That One Otter Game`) | Incomplete: D3D11 feature level 11_1 and presentation were reached, but the run coincided with a forced host reboot and is not qualification evidence |
| 2026-09-04 | RX 9060 XT, RADV 26.1.6 | Bottles 67.2/ProtoSoda 11.0-1 + DXVK 3.0.2 | deterministic DX11 window/swapchain/90-present fixture (`72aef406…16b1e`) with NeuroShade shader processing | Qualified |
| 2026-09-04 | RX 9060 XT, RADV 26.1.6 | Bottles 67.2/ProtoSoda 11.0-1 + VKD3D-Proton 3.0.1 | deterministic DX12 queue/RTV/barrier/fence/90-present fixture (`3e696ba5…949b`) with NeuroShade shader processing | Qualified |

The accepted Proton record is `build/m9-proton/final-qualification.json`
(`qualified=yes`, DX11 exit 0, DX12 exit 0). Its adjacent logs contain the real
translator versions, the selected RX 9060 XT, and
`present_processing=active backend=shader` from both executions. The fixtures
are built reproducibly with llvm-mingw 20260826 and exit themselves after 90
presents.

On multi-GPU Mesa systems, qualification must expose only the intended device.
The RX 9060 XT test configuration uses `DRI_PRIME=1002:7590!`; translator logs
must name `AMD Radeon RX 9060 XT` as the created device. See `INCIDENTS.md` for
the 2026-09-04 combined-workload incident and the resulting isolation policy.

## Anti-cheat policy

NeuroShade is intended primarily for single-player, offline, unprotected games,
and development applications. It does not hide the Vulkan layer, bypass checks,
spoof modules, patch anti-cheat software, or inject into protected games.

`neuroshade-run` refuses executable names associated with common anti-cheat
bootstrap/services by default. `NEUROSHADE_ALLOW_ANTICHEAT=1` is an explicit
user override, not a bypass; use it only when the game publisher’s policy
clearly permits third-party Vulkan layers. A game may still reject or penalize
the layer regardless of this setting.

## Known constraints

- The layer currently targets Vulkan and Vulkan translation layers. Native
  Windows D3D12 is outside v1 scope.
- Automatic resource detection is heuristic. Profiles should persist manually
  verified depth, motion, and low-resolution bindings.
- If zero-copy interop fails its startup self-test, NeuroShade visibly selects
  host staging. If neural initialization fails, shader-only/native presentation
  remains the fallback.
- The reference neural-present adapter accepts fixed same-size FP32/NCHW model
  buckets (the bundled spatial and temporal 64×64 examples). It reports
  `HOST-STAGING-FALLBACK`; arbitrary-resolution production models require a
  matching bucket. The bundled 2× temporal-SR runtime remains qualified by its
  dedicated M8 test and safely rejects a mismatched swapchain bucket.
