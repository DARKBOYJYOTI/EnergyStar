# AGENTS.md

Windows-only native C++20 app. One binary serves as UI, Windows service, and per-session foreground agent. No tests, no package manager, no external runtime.

## Build

Use GitHub Actions for verification. This machine has no CMake or Visual Studio C++ toolchain; do not expect a local build to work. Push to `master` or open a PR for a build-only run. Tag pushes and manual dispatch with `publish=true` create/update a Release.

Requires Visual Studio C++ tools + Windows SDK; CMake is not on PATH in this environment by default.

```powershell
cmake -S Native -B build -A x64
cmake --build build --config Release
# Binary: build/bin/EnergyStar.exe; use -A ARM64 for ARM64
```

Installer (CI installs Inno Setup via Chocolatey):

```powershell
& "$env:ProgramFiles(x86)\Inno Setup 6\ISCC.exe" `
  "/DAppArch=x64" "/DPayloadDir=$((Resolve-Path build/bin).Path)" `
  "/DOutputDir=$((Resolve-Path dist/setups).Path)" Package/EnergyStar.iss
```

- CI (`.github/workflows/build.yml`) builds both x64 and ARM64, packages portable + setup, and publishes a GitHub Release on `v*` tags or manual dispatch.
- Version is duplicated: `Package/EnergyStar.iss` `AppVersion` and hardcoded text in workflow release notes. Update both.
- No lint/test/format target. Verification is a Release build for both architectures.

## Entrypoint and modes

`Native/Main.cpp` dispatches argv before the UI. Flags accept `-`, `--`, and `/` prefixes.

| Flag | Action |
| :--- | :--- |
| none | Win32 property sheet UI |
| `--service` | SCM dispatcher (`EnergyStarService`) |
| `--agent` | per-session foreground watcher |
| `--install` / `--uninstall` | create/remove auto-start service |
| `--start` / `--stop` | control service |

Service image path is `"<exe>" --service`. Do not split into separate executables; the installer/service assume one binary.

## Architecture notes

- `Native/Service/Service.cpp`: service main loop ticks every 500 ms. It throttles processes in active WTS sessions via EcoQoS + `IDLE_PRIORITY_CLASS`, except the foreground process and its descendants, user whitelist, and a hardcoded bypass list. Process identity is PID + creation time; preserve this check to avoid restoring a recycled PID.
- `Native/Agent/Agent.cpp`: service launches one agent per active session with `WTSQueryUserToken` + `CreateProcessAsUser`, assigned to a kill-on-close job object. Duplicate-instance detection uses mutex `Global\EnergyStar.Agent.<sessionId>`; the service uses the same name to detect a live agent. Renaming one side breaks agent liveness.
- `Native/Shared/Protocol.h`: named pipe `\\.\pipe\EnergyStar.Control`.
  - `Status` and `Foreground` requests/responses use only `kHeaderSize`; `Configure` uses the full `PipeMessage`.
  - Response field `foregroundPid` is a success flag (`1`/`0`), not the foreground PID.
  - Max 64 whitelist entries; changing struct layout breaks both UI and service.
- Config is stored beside the executable, not in `%ProgramData%`: `EnergyStar.conf`, `whitelist.conf`, optional `EnergyStar.log`. Service migrates legacy files once from `%ProgramData%\EnergyStar`.
- Whitelist accepts only absolute `.exe` paths. UI resolves `.lnk` shortcuts before sending.
- Desktops without a battery coerce `Automatic` mode to `AlwaysOn`.
- `Native/UI/UI.cpp` communicates with the service only through the pipe. UI must handle service not installed/stopped instead of reading config files directly.
- UI resources are generated: CMake `configure_file` copies `UI.rc.in`, `Resource.h`, `EnergyStar.rc.in`, and the manifest into the build dir. New dialog controls need both `UI.rc.in` change and a `Resource.h` ID.

## Constraints

- Keep it dependency-free: static CRT (`/MT`) and Win32/COM only. Current flags: `/MT /O1 /Os /Gy /Gw /W4 /EHsc /utf-8`.
- `.rc` files live under `Native/UI` and `Native/Assets`; generated copies go to the CMake binary dir, which is also an include path.
