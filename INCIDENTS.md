# NeuroShade qualification incidents

## 2026-09-04 — Desktop became unresponsive during combined GPU qualification

Status: cause not proven; unsafe workload combination removed and replacement workflow qualified.

### What was running

- Native Vulkan testbed soak and MIGraphX temporal soak concurrently on the RX 9060 XT.
- Bottles 67.2 `NeuroShade-M9` qualification using ProtoSoda 11.0-1 and DXVK 3.0.2.
- Unreal Engine DX11 workload at 640x360, with the NeuroShade pass-through/tracking Layer enabled.

### Persisted evidence

- DXVK created D3D11 feature level 11_1 devices for both the RX 9060 XT and the Renoir iGPU while enumerating adapters; the final presenter used the RX 9060 XT.
- NeuroShade loaded the schema-v2 profile and tracked swapchains at 640x360, 1145x621, and 1167x759. At the time of the incident, that build submitted no GPU processing work because its product Layer was still pass-through/tracking-only; M9 subsequently connected presentation processing.
- No kernel OOM, `amdgpu` ring timeout, GPU reset, device-lost error, coredump, or NeuroShade fatal error was persisted before the physical reboot.
- The Bottles scope peaked at 2.9 GiB. The host has 11 GiB usable RAM and 4 GiB zram swap.
- The interrupted soaks recorded 4,389 successful testbed iterations and 4,887 successful temporal invocations, but no final result files; they are not accepted as passed gates.

The journal records session termination at the physical reboot. Consequently, the later GNOME/Wayland disconnects are effects of shutdown and do not establish the initiating fault.

### Corrective actions

- GPU soaks acquire a shared qualification lock; Proton qualification requires the exclusive form of the same lock.
- Full testbed and temporal soaks run sequentially by default. Parallel execution requires explicit `NEUROSHADE_SOAK_PARALLEL=1` opt-in.
- Proton cases stop after the first failure by default and use `timeout --kill-after=10s`.
- Multi-GPU qualification must expose only PCI device `1002:7590` (`DRI_PRIME=1002:7590!`) and verify the selected adapter in translator logs.
- Proton qualification resumed only after both full isolated native gates completed.

## 2026-09-04 — Bottles CLI timeout left game descendants alive

Status: resolved; the affected launcher scripts were removed and the replacement cgroup runner passed real DX11/DX12 qualification.

Three `Otter-Win64-Shipping.exe` processes were found concurrently after
sequential baseline attempts. GNU `timeout` terminated the wrapper command,
but two Flatpak/Bottles process trees and their Wine children survived. Earlier
checks incorrectly concluded that no Wine server remained because
`bottles-cli stop` itself was still waiting.

`flatpak kill com.usebottles.bottles`, followed by the dedicated prefix's
`wineserver -k`, removed every Bottles/Wine/game process. No AMDGPU reset,
ring timeout, device loss, or OOM was logged during the event.

Corrective policy:

- Do not use the discarded machine-local Bottles launchers.
- The replacement Proton runner places the complete Wine process tree in a
  killable scope and verifies zero matching game/Wine processes both before
  and after every case.
- A wrapper timeout is always a failed/incomplete case and cleanup must be
  independently verified; `bottles-cli stop` output alone is insufficient.

Resolution evidence:

- `run-contained-wine.sh` rejects a live process using the dedicated prefix,
  creates one transient systemd user service with `KillMode=control-group` and
  `RuntimeMaxSec`, then verifies zero prefix processes after exit.
- `proton.contained_runner` proves descendant cleanup after an injected timeout.
- Deterministic DX11 and DX12 fixtures self-exit after 90 presents; the final
  sequential record reports both exits as zero and both translators on the RX
  9060 XT. No Bottles CLI wrapper is used.
