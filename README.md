# EnergyStar

EnergyStar is a Windows utility that uses the Windows EcoQoS API to reduce CPU impact for inactive user applications. It restores the original process priority when an application becomes active.

This repository is a community fork of [imbushuo/EnergyStar](https://github.com/imbushuo/EnergyStar), created to maintain and improve the project. Original project and code are by Bingxing Wang. See [LICENSE](LICENSE) for license terms and notices.

## Requirements

- Windows 11, build 22000 or later
- .NET 10 SDK with Windows desktop targeting packs

## Build locally

Restore and publish the x64 application:

```powershell
dotnet restore EnergyStar.sln
dotnet publish EnergyStar.UI/EnergyStar.UI.csproj -c Release -r win-x64 --self-contained true -p:PublishSingleFile=true -p:PublishReadyToRun=true
```

For ARM64, replace `win-x64` with `win-arm64`. The engine is a separate class library; the WPF UI references it without third-party UI dependencies.

Run `EnergyStar.UI.exe --debug` to enable timestamped INFO, WARN, and ERROR entries in `EnergyStar.log` beside the executable. The UI can also toggle logging. Normal runs do not write a log file.

## Modes and controls

- **Auto:** throttle on battery; restore managed apps when AC power connects.
- **Always on:** force throttling on battery and AC power.
- **Off:** stop throttling and restore managed process priorities.
- **Exclusions:** browse for executables that EnergyStar must not throttle.
- **Theme:** choose system, light, or dark mode.
- **Startup:** register or remove a highest-available Task Scheduler logon task. Windows asks for administrator approval. The task XML is saved in the system's shared application-data folder. Elevated privileges apply only to accounts that can run elevated.

## GitHub Actions

Open **Actions**, select **Build EnergyStar**, then choose **Run workflow**. Builds do not start on pushes. Successful runs upload portable ZIPs and install-ready ZIPs for x64 and ARM64.

The install-ready ZIP includes `Install-EnergyStar.ps1` and `Uninstall-EnergyStar.ps1`. Run both scripts as administrator. Installation copies the app to the system's Program Files folder. Uninstaller preserves `EnergyStar.log` and the startup task XML.

## Known limitations

- Windows does not expose a supported API to query whether an application manages its own EcoQoS state; EnergyStar cannot reliably detect that claim.
- Windows does not expose a supported API to read another app's EcoQoS state. Use Exclusions for apps that manage their own power behavior.
- The install-ready package creates a machine-wide Program Files installation; it does not create a signed MSI.

## License

This project is licensed under the MIT-style terms in [LICENSE](LICENSE). Keep the original copyright and permission notice in copies or substantial portions. The license includes a separate Microsoft employee/vendor/contractor provision; consult the license text for details.
