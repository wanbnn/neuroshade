# Proton qualification

The release gate requires real, separately recorded DX11 to DXVK and DX12 to
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

`qualify-proton.sh` only emits `qualified=yes` when both launchers exit zero and
their captured logs explicitly contain the matching translator names (`DXVK`
and `VKD3D-Proton`) plus `present_processing=active backend=shader` from the
NeuroShade Layer. Set `NEUROSHADE_EXPECTED_GPU='AMD Radeon RX 9060 XT'` to also
require the intended GPU in both logs.

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

The installed `windows/` sources build two self-terminating PE64 fixtures with
the official llvm-mingw 20260826 Linux archive. The archive used for the local
qualification has SHA-256
`868f11a74eeebd8efca3fcba55cfad8c78829524350bc88cef3875a898cbab8b`.

```bash
LLVM_MINGW_ROOT=/path/to/llvm-mingw \
  ./build-windows-smokes.sh ./workloads
```

`--no-insert-timestamp` makes repeated builds byte-identical. The accepted
fixture hashes are:

- DX11: `72aef4067e67a4971747f5e44d3540940ca6ef560cfacc5b7e60b6a8ef916b1e`
- DX12: `3e696ba58f3d7feafaa26216b9a24790667a4307b2eea85899300fd46749949b`
