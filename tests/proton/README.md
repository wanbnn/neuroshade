# Proton qualification

The release gate requires real, separately recorded DX9 and DX11 to DXVK and DX12 to
VKD3D-Proton executions. `proton.harness_contract` only tests orchestration and
must never be reported as game qualification.

## Safety preflight

1. Use an unprotected, offline workload. Never use an anti-cheat bootstrap.
2. Use a dedicated prefix/bottle. Do not modify a user's normal game bottle.
3. Run native/HIP soaks and Proton exclusively; all bundled qualification
   scripts enforce the same `flock` lock.
4. On this dual-AMD host, set `DRI_PRIME=1002:7590!`. The trailing `!` makes
   Mesa expose only the RX 9060 XT to Vulkan.
5. First run a short baseline without NeuroShade. Then enable the explicit
   Layer for a separate run.
6. Execute Wine through `run-contained-wine.sh`. It rejects a live process in
   the same prefix, runs one transient systemd user service with
   `KillMode=control-group`, and verifies zero prefix processes after exit.
7. Use a workload that exits itself. For packaged Unreal applications,
   `-seconds=N` sets a maximum tick time and exits even when benchmark mode is
   disabled. Keep an outer timeout with a kill grace period as containment,
   not as a success condition.

Mesa device-selection reference:
https://docs.mesa3d.org/envvars.html#envvar-DRI_PRIME

Unreal command-line reference:
https://dev.epicgames.com/documentation/unreal-engine/unreal-engine-command-line-arguments-reference

## Evidence required

`qualify-proton.sh` only emits `qualified=yes` when all three launchers exit zero and
their captured logs explicitly contain the matching translator names (`DXVK`
and `VKD3D-Proton`) plus `present_processing=active backend=shader` from the
NeuroShade Layer. DX9 additionally requires a `D3D9` API marker from the
translator, so a D3D11 DXVK log cannot satisfy its gate.
Set `NEUROSHADE_EXPECTED_GPU='AMD Radeon RX 9060 XT'` to also
require the intended GPU in all three logs.

```bash
./qualify-proton.sh --dx9-launcher ./run-dx9.sh \
  --dx11-launcher ./run-dx11.sh --dx12-launcher ./run-dx12.sh \
  --output ./qualification.json
```

Schema version 2 adds `dx9_exit`, `dx9_evidence`, `dx9_layer_evidence`, and
`dx9_log`. The DX9 launcher is now required; historical schema-v1 results
remain evidence for DX11/DX12 only. Cases run sequentially in DX9/DX11/DX12
order, stopping after a nonzero exit by default.

For each real result, retain:

- Proton/Bottles runner and build;
- DXVK or VKD3D-Proton version;
- exact workload and arguments;
- selected GPU;
- NeuroShade session log and translator log;
- duration and exit status.

Timeout, device loss, desktop/session reset, missing translator evidence, or an
unexpected GPU is a failed/incomplete run, never a qualified run.

## Reproducible qualification fixtures

The installed `windows/` sources build three self-terminating PE64 fixtures with
the official llvm-mingw 20260826 Linux archive. The archive used for the local
qualification has SHA-256
`868f11a74eeebd8efca3fcba55cfad8c78829524350bc88cef3875a898cbab8b`.

```bash
LLVM_MINGW_ROOT=/path/to/llvm-mingw \
  ./build-windows-smokes.sh ./workloads
```

`--no-insert-timestamp` makes repeated builds byte-identical. The accepted
fixture hashes are:

- DX9: `cf3427cc09110606cda3cce3ff85a4d7bc10155d7345da040ab2921ef1bcfa89`
- DX11: `72aef4067e67a4971747f5e44d3540940ca6ef560cfacc5b7e60b6a8ef916b1e`
- DX12: `3e696ba58f3d7feafaa26216b9a24790667a4307b2eea85899300fd46749949b`

The DX9 fixture presents 90 frames and calls `IDirect3DDevice9::Reset` midway
with a new backbuffer size. Early closure, failed reset, or failed presentation
returns nonzero. Its completion marker is
`d3d9_smoke=pass presents=90 resets=1`.

Run it first without the layer, then through `neuroshade-run` with a shader
profile, using separate log directories for each execution:

```bash
./run-bottles-smoke.sh --api dx9 \
  --workload /absolute/path/workloads/neuroshade-dx9-smoke.exe \
  --prefix /absolute/path/dedicated-bottle \
  --runner-bin /absolute/path/runner/files/bin \
  --log-dir /absolute/path/logs/dx9-baseline --timeout 30
```

This Bottles fixture runner retains the RX 9060 XT selection used by the
existing DX11/DX12 tests. DX9 uses DXVK logging (`*_d3d9.log`) and a
process-local `d3d9=n` override; the dedicated bottle must already contain
DXVK's DLL. Installation details are in the
[DXVK documentation](https://github.com/doitsujin/dxvk/blob/master/README.md).

The distributed NeuroShade layer and these fixtures are x86_64. D3D9 games
are often 32-bit: a traditional 32-bit Wine/Vulkan process needs a matching
32-bit NeuroShade layer, which is not currently shipped. Wine's WoW64 path
must be qualified separately; a PE64 fixture does not establish PE32 support.
These tests do not qualify D3D9Ex, exclusive fullscreen, or lost-device recovery.
