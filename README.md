# EnergyStar

EnergyStar is a lightweight Windows power utility. A native Windows service monitors processes; a small per-session helper reports foreground changes. The native UI runs only when opened.

## Components

- `EnergyStar.Service.exe`: Windows service running as LocalSystem. Windows restarts it after crashes. A manual stop stays stopped.
- `EnergyStar.Agent.exe`: tiny hidden per-session foreground-event helper. It has no UI and sends only process IDs through a local authenticated pipe.
- `EnergyStar.exe`: on-demand native Win32 settings UI. It needs no administrator permission after setup.

Service shutdown restores process priorities and disables EcoQoS for processes managed during that run. Protected Windows processes may still deny access.

## Configuration

Service stores `EnergyStar.conf` and `whitelist.conf` under Windows CommonApplicationData in the `EnergyStar` folder. UI sends validated settings through a local-only pipe; service writes the files and applies changes without restarting.

`EnergyStar.conf` supports `mode=auto`, `mode=always`, `mode=off`, and `log=on/off`. `whitelist.conf` stores one executable path per line. Logging is off by default.

## Install and portable builds

Run matching `EnergyStar-Setup-win-x64.exe` or `EnergyStar-Setup-win-ARM64.exe` as administrator. Setup installs and starts the service, creates shortcuts, and configures service crash recovery. UI and service settings changes need no elevation.

Portable ZIPs contain the three native executables. Installing the service from a portable folder requires administrator permission.

GitHub Actions builds only when manually dispatched. It exports separate setup executables and portable ZIPs for x64 and ARM64.

## Build locally

Use Visual Studio 2022 Build Tools with the C++ workload and Windows 11 SDK. Then run:

```powershell
cmake -S Native -B build -A x64
cmake --build build --config Release
```

## Limitations

Windows does not provide a supported API to read another application’s EcoQoS state. Add those apps to the whitelist.

## License

See [LICENSE](LICENSE). Preserve original copyright and license notices.
