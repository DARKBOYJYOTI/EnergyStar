# EnergyStar

EnergyStar is a lightweight, native Windows power utility designed to optimize background applications without compromising foreground responsiveness. A single native executable hosts the Win32 UI, the background Windows service, and the per-session foreground event helper.

Made with ❤️ by [darkboyjyoti](https://github.com/DARKBOYJYOTI/EnergyStar)  
Based on the original CLI concept by [imbushuo](https://github.com/imbushuo/EnergyStar)

---

## Features

- **EcoQoS & Process Priority Management**: Automatically puts background applications into Windows EcoQoS (Efficiency Mode) and idle priority while dynamically boosting the active foreground application and its child processes.
- **Three Power Modes**:
  - `Auto - battery only`: Power optimization activates only when running on battery power.
  - `Always on`: Active at all times (both AC power and battery).
  - `Off`: Power control completely disabled.
- **Smart Whitelist**:
  - Exclude critical background tasks (compilers, video renderers, games, audio tools).
  - Browse installed applications easily with automatic shortcut (`.lnk`) resolution to the target executable.
  - Full horizontal and vertical scrollbars for long paths.
  - Right-click context menu and `Ctrl+C` support to copy application paths.
- **Structured Debug Logging**:
  - Disabled by default for maximum performance.
  - Formatted with ISO timestamp, local timezone, process name, PID, boost/throttle events, and warnings.
- **Dedicated Help & About Tabs**:
  - Full in-app user guide and documentation.
  - Clickable repository links, credits, privacy policy, and MIT license disclaimer.
- **Robust Service Management**:
  - Install, start, stop, or remove the background service with one click directly from the UI.
  - Installer and uninstaller automatically stop the service and terminate running processes to avoid file-in-use errors.

---

## Architecture

- `EnergyStar.exe --service`: Windows service running as `LocalSystem` in Session 0. Signed-in users can start and stop it without administrator rights.
- `EnergyStar.exe --agent`: Lightweight hidden per-session helper that runs in each active user desktop session to track foreground window focus changes via `SetWinEventHook`.
- `EnergyStar.exe`: On-demand native Win32 settings UI with property sheet tabs (Power, Whitelist, Settings, Help, About).

> [!NOTE]
> When EnergyStar is installed, Task Manager will display two `EnergyStar.exe` entries under background processes: the background Windows Service (Session 0) and the user-session foreground tracking agent. This is normal Windows architecture due to Session 0 isolation. Agents are tracked via a Windows Job Object and terminate automatically when the service stops.

---

## Configuration

All builds store `EnergyStar.conf`, `whitelist.conf`, and `EnergyStar.log` beside the executable:
- **Installed**: `C:\Program Files\EnergyStar`
- **Portable**: Folder beside the executable

The UI sends validated settings through a local authenticated named pipe; the service writes configuration files and applies changes dynamically without requiring a restart.

---

## Downloads

GitHub Actions automatically builds single `.exe` binaries for both x64 and ARM64:

- **Setup Installer**: `EnergyStar-Setup-win-x64.exe` / `EnergyStar-Setup-win-ARM64.exe`
- **Standalone Portable**: `EnergyStar-portable-x64.exe` / `EnergyStar-portable-ARM64.exe`

---

## Build Locally

Requirements: Visual Studio 2022 Build Tools (C++ workload) and Windows 11 SDK.

```powershell
cmake -S Native -B build -A x64
cmake --build build --config Release
```

---

## License & Disclaimer

Released under the [MIT License](LICENSE).

```text
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
