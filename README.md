# EnergyStar

EnergyStar is a lightweight Windows power utility. One native executable hosts the UI, Windows service, and per-session foreground helper. The UI runs only when opened.

## Components

- `EnergyStar.exe --service`: Windows service running as LocalSystem. Signed-in users can start and stop it from Services; a manual stop stays stopped.
- `EnergyStar.exe --agent`: tiny hidden per-session foreground-event helper. One runs for each active user session. It has no UI and sends only process IDs through a local authenticated pipe.
- `EnergyStar.exe`: on-demand native Win32 settings UI. It needs no administrator permission after setup and opens one window per user session.

Service shutdown restores process priorities and disables EcoQoS for processes managed during that run. Protected Windows processes may still deny access.

## Configuration

All builds store `EnergyStar.conf`, `whitelist.conf`, and `EnergyStar.log` beside the executable. Setup installs them under `C:\Program Files\EnergyStar`; portable builds keep them beside the portable executable. UI sends validated settings through a local-only pipe; service writes the files and applies changes without restarting. Uninstall preserves these user settings and logs.

`EnergyStar.conf` supports `mode=auto`, `mode=always`, `mode=off`, and `log=on/off`. `whitelist.conf` stores one executable path per line. Logging is off by default.

## Install and portable builds

Run matching `EnergyStar-Setup-win-x64.exe` or `EnergyStar-Setup-win-ARM64.exe` as administrator. Setup installs and starts the service and creates shortcuts. To use a portable executable, run `EnergyStar-portable-x64.exe --install` as administrator once. All signed-in Windows users can use the shared service and settings; any user can stop or start the service in `services.msc`. A manual stop stays stopped until a user starts it or Windows reboots.

Portable downloads are single `.exe` files. Their settings and logs stay beside that executable.

GitHub Actions builds only when manually dispatched. It exports setup and single-file portable executables for x64 and ARM64.

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
