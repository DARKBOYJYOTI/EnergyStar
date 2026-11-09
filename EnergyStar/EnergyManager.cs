using EnergyStar.Interop;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

namespace EnergyStar
{
    public class EnergyManager
    {
        public static readonly HashSet<string> BypassProcessList = new HashSet<string>
        {
            // Not ourselves
            "EnergyStar.exe".ToLowerInvariant(),
            "EnergyStar.Core.exe".ToLowerInvariant(),
            // Edge has energy awareness
            "msedge.exe",
            "WebViewHost.exe".ToLowerInvariant(),
            // UWP Frame has special handling, should not be throttled
            "ApplicationFrameHost.exe".ToLowerInvariant(),
            // Fire extinguisher should not catch fire
            "taskmgr.exe",
            "procmon.exe",
            "procmon64.exe",
            // Widgets
            "Widgets.exe".ToLowerInvariant(),
            // System shell
            "dwm.exe",
            "explorer.exe",
            "ShellExperienceHost.exe".ToLowerInvariant(),
            "StartMenuExperienceHost.exe".ToLowerInvariant(),
            "SearchHost.exe".ToLowerInvariant(),
            "sihost.exe",
            "fontdrvhost.exe",
            // IME
            "ChsIME.exe".ToLowerInvariant(),
            "ctfmon.exe",
#if DEBUG
            // Visual Studio
            "devenv.exe",
#endif
            // System Service - they have their awareness
            "csrss.exe",
            "smss.exe",
            "svchost.exe",
            // WUDF
            "WUDFRd.exe".ToLowerInvariant(),
        };
        // Speical handling needs for UWP to get the child window process
        public const string UWPFrameHostApp = "ApplicationFrameHost.exe";

        private static uint pendingProcPid = 0;
        private static string pendingProcName = "";
        private static readonly object Sync = new();
        private static readonly Dictionary<uint, ManagedProcess> ManagedProcesses = new();
        private static HashSet<string> excludedExecutables = new(StringComparer.OrdinalIgnoreCase);
        private static Timer? scanTimer;
        private static EnergyMode mode = EnergyMode.Auto;
        private static bool onBattery;
        private static bool policyEnabled;

        private sealed class ManagedProcess
        {
            public required string Name { get; init; }
            public required Win32Api.PriorityClass OriginalPriority { get; init; }
        }

        public static event Action<EnergyStatus>? StatusChanged;

        public static void Start(EnergyMode initialMode, IEnumerable<string> exclusions, bool loggingEnabled)
        {
            lock (Sync)
            {
                Logger.Configure(loggingEnabled);
                mode = initialMode;
                excludedExecutables = NormalizeExclusions(exclusions);
                HookManager.SubscribeToWindowEvents();
                RefreshPowerPolicy();
                if (policyEnabled)
                {
                    HandleForegroundEventCore(Win32Api.GetForegroundWindow());
                    ThrottleAllUserBackgroundProcessesCore();
                }
                scanTimer ??= new Timer(_ => ScanTimerTick(), null, TimeSpan.FromSeconds(10), TimeSpan.FromSeconds(10));
                PublishStatus();
                Logger.Info("EnergyStar engine started.");
            }
        }

        public static void Configure(EnergyMode newMode, IEnumerable<string> exclusions, bool loggingEnabled)
        {
            lock (Sync)
            {
                mode = newMode;
                excludedExecutables = NormalizeExclusions(exclusions);
                Logger.Configure(loggingEnabled);
                var wasEnabled = policyEnabled;
                RefreshPowerPolicy();
                if (wasEnabled && !policyEnabled)
                {
                    RestoreManagedProcesses();
                    pendingProcPid = 0;
                    pendingProcName = "";
                }
                else if (!wasEnabled && policyEnabled)
                {
                    HandleForegroundEventCore(Win32Api.GetForegroundWindow());
                    ThrottleAllUserBackgroundProcessesCore();
                }
                else if (policyEnabled)
                {
                    RestoreExcludedProcesses();
                    ThrottleAllUserBackgroundProcessesCore();
                }
                PublishStatus();
            }
        }

        public static void Stop()
        {
            lock (Sync)
            {
                scanTimer?.Dispose();
                scanTimer = null;
                policyEnabled = false;
                HookManager.UnsubscribeWindowEvents();
                RestoreManagedProcesses();
                pendingProcPid = 0;
                pendingProcName = "";
                Logger.Info("EnergyStar engine stopped.");
                Logger.Close();
            }
        }

        public static EnergyStatus GetStatus()
        {
            lock (Sync)
                return new EnergyStatus { Mode = mode, OnBattery = onBattery, IsEnabled = policyEnabled,
                    ManagedProcessCount = ManagedProcesses.Count };
        }

        private static HashSet<string> NormalizeExclusions(IEnumerable<string> paths) => new(
            paths.Where(path => !string.IsNullOrWhiteSpace(path)).Select(Path.GetFullPath), StringComparer.OrdinalIgnoreCase);

        private static void ScanTimerTick()
        {
            lock (Sync)
            {
                var wasEnabled = policyEnabled;
                RefreshPowerPolicy();
                if (wasEnabled && !policyEnabled)
                {
                    RestoreManagedProcesses();
                    pendingProcPid = 0;
                    pendingProcName = "";
                }
                else if (!wasEnabled && policyEnabled) HandleForegroundEventCore(Win32Api.GetForegroundWindow());
                if (policyEnabled) ThrottleAllUserBackgroundProcessesCore();
                PublishStatus();
            }
        }

        private static void RefreshPowerPolicy()
        {
            onBattery = Win32Api.GetSystemPowerStatus(out var status) && status.ACLineStatus == 0;
            var enabled = mode == EnergyMode.AlwaysOn || (mode == EnergyMode.Auto && onBattery);
            if (enabled != policyEnabled && Logger.IsEnabled)
                Logger.Info($"Power policy changed: mode={mode}, onBattery={onBattery}, enabled={enabled}.");
            policyEnabled = enabled;
        }

        private static void PublishStatus() => StatusChanged?.Invoke(new EnergyStatus
        {
            Mode = mode,
            OnBattery = onBattery,
            IsEnabled = policyEnabled,
            ManagedProcessCount = ManagedProcesses.Count
        });

        private static readonly IntPtr pThrottleOn;
        private static readonly IntPtr pThrottleOff;
        private static readonly int szControlBlock;

        static EnergyManager()
        {
            szControlBlock = Marshal.SizeOf<Win32Api.PROCESS_POWER_THROTTLING_STATE>();
            pThrottleOn = Marshal.AllocHGlobal(szControlBlock);
            pThrottleOff = Marshal.AllocHGlobal(szControlBlock);

            var throttleState = new Win32Api.PROCESS_POWER_THROTTLING_STATE
            {
                Version = Win32Api.PROCESS_POWER_THROTTLING_STATE.PROCESS_POWER_THROTTLING_CURRENT_VERSION,
                ControlMask = Win32Api.ProcessorPowerThrottlingFlags.PROCESS_POWER_THROTTLING_EXECUTION_SPEED,
                StateMask = Win32Api.ProcessorPowerThrottlingFlags.PROCESS_POWER_THROTTLING_EXECUTION_SPEED,
            };

            var unthrottleState = new Win32Api.PROCESS_POWER_THROTTLING_STATE
            {
                Version = Win32Api.PROCESS_POWER_THROTTLING_STATE.PROCESS_POWER_THROTTLING_CURRENT_VERSION,
                ControlMask = Win32Api.ProcessorPowerThrottlingFlags.PROCESS_POWER_THROTTLING_EXECUTION_SPEED,
                StateMask = Win32Api.ProcessorPowerThrottlingFlags.None,
            };

            Marshal.StructureToPtr(throttleState, pThrottleOn, false);
            Marshal.StructureToPtr(unthrottleState, pThrottleOff, false);
        }

        private static bool ToggleEfficiencyMode(IntPtr hProcess, uint processId, string processName, bool enable)
        {
            if (hProcess == IntPtr.Zero) return false;

            if (ManagedProcesses.TryGetValue(processId, out var previous) &&
                !previous.Name.Equals(processName, StringComparison.OrdinalIgnoreCase))
                ManagedProcesses.Remove(processId);

            if (enable)
            {
                if (!ManagedProcesses.ContainsKey(processId))
                {
                    var originalPriority = Win32Api.GetPriorityClass(hProcess);
                    if (originalPriority == 0)
                    {
                        Logger.Warning($"GetPriorityClass failed for {processName} (PID {processId}, error {Marshal.GetLastWin32Error()}).");
                        return false;
                    }
                    ManagedProcesses[processId] = new ManagedProcess { Name = processName, OriginalPriority = originalPriority };
                }
            }
            else if (!ManagedProcesses.TryGetValue(processId, out _))
            {
                return false;
            }

            if (!Win32Api.SetProcessInformation(hProcess, Win32Api.PROCESS_INFORMATION_CLASS.ProcessPowerThrottling,
                enable ? pThrottleOn : pThrottleOff, (uint)szControlBlock))
            {
                Logger.Warning($"SetProcessInformation failed for {processName} (PID {processId}, error {Marshal.GetLastWin32Error()}).");
            }

            var priority = enable ? Win32Api.PriorityClass.IDLE_PRIORITY_CLASS : ManagedProcesses[processId].OriginalPriority;
            if (!Win32Api.SetPriorityClass(hProcess, priority))
            {
                Logger.Warning($"SetPriorityClass failed for {processName} (PID {processId}, error {Marshal.GetLastWin32Error()}).");
            }
            if (!enable) ManagedProcesses.Remove(processId);
            return true;
        }

        private static string GetProcessNameFromHandle(IntPtr hProcess)
        {
            int capacity = 1024;
            var sb = new StringBuilder(capacity);

            if (Win32Api.QueryFullProcessImageName(hProcess, 0, sb, ref capacity))
            {
                return Path.GetFileName(sb.ToString());
            }

            Logger.Warning($"QueryFullProcessImageName failed (error {Marshal.GetLastWin32Error()}).");
            return "";
        }

        public static void HandleForegroundEvent(IntPtr hwnd)
        {
            lock (Sync) HandleForegroundEventCore(hwnd);
        }

        private static bool IsExcluded(IntPtr processHandle, string processName)
        {
            var executableName = Path.GetFileName(processName);
            if (!executableName.EndsWith(".exe", StringComparison.OrdinalIgnoreCase))
                executableName += ".exe";
            if (BypassProcessList.Contains(executableName.ToLowerInvariant())) return true;
            var capacity = 1024;
            var path = new StringBuilder(capacity);
            if (!Win32Api.QueryFullProcessImageName(processHandle, 0, path, ref capacity))
            {
                if (excludedExecutables.Count > 0 && Logger.IsEnabled)
                    Logger.Warning($"Could not check exclusion path for {processName} (error {Marshal.GetLastWin32Error()}).");
                return excludedExecutables.Count > 0;
            }
            return excludedExecutables.Contains(Path.GetFullPath(path.ToString()));
        }

        private static void HandleForegroundEventCore(IntPtr hwnd)
        {
            if (!policyEnabled) return;

            var windowThreadId = Win32Api.GetWindowThreadProcessId(hwnd, out uint procId);
            // This is invalid, likely a process is dead, or idk
            if (windowThreadId == 0 || procId == 0)
            {
                if (Logger.IsEnabled) Logger.Warning("Foreground window has no valid process ID.");
                return;
            }

            var procHandle = Win32Api.OpenProcess(
                (uint) (Win32Api.ProcessAccessFlags.QueryLimitedInformation | Win32Api.ProcessAccessFlags.SetInformation), false, procId);
            if (procHandle == IntPtr.Zero)
            {
                if (Logger.IsEnabled) Logger.Warning($"Could not open foreground process {procId} (error {Marshal.GetLastWin32Error()}).");
                return;
            }

            // Get the process
            var appName = GetProcessNameFromHandle(procHandle);
            
            try
            {
                // UWP needs to be handled in a special case
                if (appName.Equals(UWPFrameHostApp, StringComparison.OrdinalIgnoreCase))
                {
                    var found = false;
                    Win32Api.EnumChildWindows(hwnd, (innerHwnd, lparam) =>
                    {
                        if (found) return true;
                        if (Win32Api.GetWindowThreadProcessId(innerHwnd, out uint innerProcId) > 0 && procId != innerProcId)
                        {
                            var innerProcHandle = Win32Api.OpenProcess((uint)(Win32Api.ProcessAccessFlags.QueryLimitedInformation |
                                Win32Api.ProcessAccessFlags.SetInformation), false, innerProcId);
                            if (innerProcHandle == IntPtr.Zero) return true;

                            Win32Api.CloseHandle(procHandle);
                            procHandle = innerProcHandle;
                            procId = innerProcId;
                            appName = GetProcessNameFromHandle(procHandle);
                            found = true;
                        }

                        return true;
                    }, IntPtr.Zero);
                }

            // Boost the current foreground app, and then impose EcoQoS for previous foreground app
            var bypass = string.IsNullOrEmpty(appName) || IsExcluded(procHandle, appName);
            if (!bypass)
            {
                if (Logger.IsEnabled) Logger.Info($"Foreground: boosting {appName} (PID {procId}).");
                ToggleEfficiencyMode(procHandle, procId, appName, false);
                BoostChildProcesses(procId);
            }

            if (pendingProcPid != 0)
            {
                if (Logger.IsEnabled) Logger.Info($"Foreground changed: throttling {pendingProcName} (PID {pendingProcPid}).");

                var prevProcHandle = Win32Api.OpenProcess((uint)(Win32Api.ProcessAccessFlags.QueryLimitedInformation |
                    Win32Api.ProcessAccessFlags.SetInformation), false, pendingProcPid);
                if (prevProcHandle != IntPtr.Zero)
                {
                    try
                    {
                        if (!IsExcluded(prevProcHandle, pendingProcName))
                            ToggleEfficiencyMode(prevProcHandle, pendingProcPid, pendingProcName, true);
                        else
                            ToggleEfficiencyMode(prevProcHandle, pendingProcPid, pendingProcName, false);
                    }
                    finally
                    {
                        Win32Api.CloseHandle(prevProcHandle);
                    }
                }
                else if (Logger.IsEnabled)
                {
                    Logger.Warning($"Could not open previous foreground process {pendingProcName} (PID {pendingProcPid}, error {Marshal.GetLastWin32Error()}).");
                }
                pendingProcPid = 0;
                pendingProcName = "";
            }

            pendingProcPid = procId;
            pendingProcName = appName;

            }
            finally
            {
                Win32Api.CloseHandle(procHandle);
            }
        }

        public static void ThrottleAllUserBackgroundProcesses()
        {
            lock (Sync) ThrottleAllUserBackgroundProcessesCore();
        }

        private static void ThrottleAllUserBackgroundProcessesCore()
        {
            if (!policyEnabled) return;
            if (Logger.IsEnabled) Logger.Info("Scanning user-session processes for background throttling.");
            using var currentProcess = Process.GetCurrentProcess();
            var activeChildPids = pendingProcPid == 0
                ? new HashSet<uint>()
                : GetChildProcesses(pendingProcPid).Select(child => child.ProcessId).ToHashSet();
            foreach (var proc in Process.GetProcesses())
            {
                using (proc)
                {
                    try
                    {
                        if (proc.SessionId != currentProcess.SessionId || proc.Id == pendingProcPid || activeChildPids.Contains((uint)proc.Id)) continue;
                        var hProcess = Win32Api.OpenProcess((uint)(Win32Api.ProcessAccessFlags.QueryLimitedInformation |
                            Win32Api.ProcessAccessFlags.SetInformation), false, (uint)proc.Id);
                        if (hProcess == IntPtr.Zero)
                        {
                            if (Logger.IsEnabled) Logger.Warning($"Could not open {proc.ProcessName} (PID {proc.Id}) for throttling (error {Marshal.GetLastWin32Error()}).");
                            continue;
                        }
                        try
                        {
                            if (IsExcluded(hProcess, proc.ProcessName))
                            {
                                if (ManagedProcesses.ContainsKey((uint)proc.Id))
                                    ToggleEfficiencyMode(hProcess, (uint)proc.Id, proc.ProcessName, false);
                                continue;
                            }
                            if (ToggleEfficiencyMode(hProcess, (uint)proc.Id, proc.ProcessName, true) && Logger.IsEnabled)
                                Logger.Info($"Throttled {proc.ProcessName} (PID {proc.Id}).");
                        }
                        finally
                        {
                            Win32Api.CloseHandle(hProcess);
                        }
                    }
                    catch (InvalidOperationException ex)
                    {
                        if (Logger.IsEnabled) Logger.Warning($"Process exited during scan: {ex.Message}");
                    }
                    catch (System.ComponentModel.Win32Exception ex)
                    {
                        if (Logger.IsEnabled) Logger.Warning($"Could not inspect process: {ex.Message}");
                    }
                }
            }

        }

        private static void RestoreExcludedProcesses()
        {
            foreach (var process in Process.GetProcesses())
            {
                using (process)
                {
                    if (!ManagedProcesses.ContainsKey((uint)process.Id)) continue;
                    var handle = Win32Api.OpenProcess((uint)(Win32Api.ProcessAccessFlags.QueryLimitedInformation |
                        Win32Api.ProcessAccessFlags.SetInformation), false, (uint)process.Id);
                    if (handle == IntPtr.Zero) continue;
                    try
                    {
                        if (IsExcluded(handle, process.ProcessName))
                            ToggleEfficiencyMode(handle, (uint)process.Id, process.ProcessName, false);
                    }
                    finally
                    {
                        Win32Api.CloseHandle(handle);
                    }
                }
            }
        }

        private static void RestoreManagedProcesses()
        {
            foreach (var process in ManagedProcesses.ToArray())
            {
                var handle = Win32Api.OpenProcess((uint)(Win32Api.ProcessAccessFlags.QueryLimitedInformation |
                    Win32Api.ProcessAccessFlags.SetInformation), false, process.Key);
                if (handle == IntPtr.Zero)
                {
                    ManagedProcesses.Remove(process.Key);
                    continue;
                }

                try
                {
                    ToggleEfficiencyMode(handle, process.Key, process.Value.Name, false);
                }
                finally
                {
                    Win32Api.CloseHandle(handle);
                }
            }
        }

        private static void BoostChildProcesses(uint rootProcessId)
        {
            using var currentProcess = Process.GetCurrentProcess();
            foreach (var child in GetChildProcesses(rootProcessId))
            {
                if (BypassProcessList.Contains(child.Name.ToLowerInvariant())) continue;

                try
                {
                    using var childProcess = Process.GetProcessById((int)child.ProcessId);
                    if (childProcess.SessionId != currentProcess.SessionId) continue;
                    var childHandle = Win32Api.OpenProcess(
                        (uint)(Win32Api.ProcessAccessFlags.QueryLimitedInformation | Win32Api.ProcessAccessFlags.SetInformation),
                        false, child.ProcessId);
                    if (childHandle == IntPtr.Zero)
                    {
                        if (Logger.IsEnabled) Logger.Warning($"Could not open child process {child.Name} (PID {child.ProcessId}, error {Marshal.GetLastWin32Error()}).");
                        continue;
                    }

                    try
                    {
                        if (ToggleEfficiencyMode(childHandle, child.ProcessId, Path.GetFileName(child.Name), false) && Logger.IsEnabled)
                            Logger.Info($"Boosted child process {child.Name} (PID {child.ProcessId}) of PID {rootProcessId}.");
                    }
                    finally
                    {
                        Win32Api.CloseHandle(childHandle);
                    }
                }
                catch (ArgumentException) { }
                catch (InvalidOperationException) { }
                catch (System.ComponentModel.Win32Exception ex)
                {
                    if (Logger.IsEnabled) Logger.Warning($"Could not inspect child process {child.Name} (PID {child.ProcessId}): {ex.Message}");
                }
            }
        }

        private static List<(uint ProcessId, string Name)> GetChildProcesses(uint rootProcessId)
        {
            const uint snapshotProcesses = 0x00000002;
            var snapshot = Win32Api.CreateToolhelp32Snapshot(snapshotProcesses, 0);
            if (snapshot == new IntPtr(-1))
            {
                Logger.Warning($"Could not enumerate child processes for PID {rootProcessId} (error {Marshal.GetLastWin32Error()}).");
                return new List<(uint ProcessId, string Name)>();
            }

            try
            {
                var childrenByParent = new Dictionary<uint, List<(uint ProcessId, string Name)>>();
                var entry = new Win32Api.PROCESSENTRY32 { dwSize = (uint)Marshal.SizeOf<Win32Api.PROCESSENTRY32>() };
                if (Win32Api.Process32First(snapshot, ref entry))
                {
                    do
                    {
                        if (!childrenByParent.TryGetValue(entry.th32ParentProcessID, out var children))
                        {
                            children = new List<(uint ProcessId, string Name)>();
                            childrenByParent[entry.th32ParentProcessID] = children;
                        }
                        children.Add((entry.th32ProcessID, entry.szExeFile));
                        entry.dwSize = (uint)Marshal.SizeOf<Win32Api.PROCESSENTRY32>();
                    }
                    while (Win32Api.Process32Next(snapshot, ref entry));
                }

                var visited = new HashSet<uint> { rootProcessId };
                var pending = new Queue<uint>();
                var descendants = new List<(uint ProcessId, string Name)>();
                pending.Enqueue(rootProcessId);
                while (pending.Count > 0)
                {
                    var parentId = pending.Dequeue();
                    if (!childrenByParent.TryGetValue(parentId, out var children)) continue;
                    foreach (var child in children)
                    {
                        if (!visited.Add(child.ProcessId)) continue;
                        pending.Enqueue(child.ProcessId);
                        descendants.Add(child);
                    }
                }
                return descendants;
            }
            finally
            {
                Win32Api.CloseHandle(snapshot);
            }
        }
    }
}
