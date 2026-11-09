using System.ComponentModel;
using System.Runtime.InteropServices;

namespace EnergyStar.Interop
{
    internal class HookManager
    {
        private const int WINEVENT_INCONTEXT = 4;
        private const int WINEVENT_OUTOFCONTEXT = 0;
        private const int WINEVENT_SKIPOWNPROCESS = 2;
        private const int WINEVENT_SKIPOWNTHREAD = 1;

        private const int EVENT_SYSTEM_FOREGROUND = 3;

        private static IntPtr windowEventHook;
        // Explicitly declare it to prevent GC
        // See: https://stackoverflow.com/questions/6193711/call-has-been-made-on-garbage-collected-delegate-in-c
        private static WinEventProc hookProcDelegate = WindowEventCallback;

        public static void SubscribeToWindowEvents()
        {
            if (windowEventHook == IntPtr.Zero)
            {
                windowEventHook = SetWinEventHook(
                    EVENT_SYSTEM_FOREGROUND, // eventMin
                    EVENT_SYSTEM_FOREGROUND, // eventMax
                    IntPtr.Zero,             // hmodWinEventProc
                    hookProcDelegate,        // lpfnWinEventProc
                    0,                       // idProcess
                    0,                       // idThread
                    WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

                if (windowEventHook == IntPtr.Zero)
                {
                    var error = Marshal.GetLastWin32Error();
                    Logger.Error($"SetWinEventHook failed (error {error}).");
                    throw new Win32Exception(error);
                }
                if (Logger.IsEnabled) Logger.Info("Foreground event hook registered.");
            }
        }

        public static void UnsubscribeWindowEvents()
        {
            if (windowEventHook != IntPtr.Zero)
            {
                if (UnhookWinEvent(windowEventHook) == 0 && Logger.IsEnabled)
                {
                    Logger.Warning($"UnhookWinEvent failed (error {Marshal.GetLastWin32Error()}).");
                }
                windowEventHook = IntPtr.Zero;
            }
        }

        public static void WindowEventCallback(IntPtr hWinEventHook, uint eventType,
            IntPtr hwnd, int idObject, int idChild, uint dwEventThread, uint dwmsEventTime)
        {
            try
            {
                EnergyManager.HandleForegroundEvent(hwnd);
            }
            catch (Exception ex)
            {
                Logger.Error("Foreground event callback failed.", ex);
            }
        }

        public delegate void WinEventProc(IntPtr hWinEventHook, uint eventType,
            IntPtr hwnd, int idObject, int idChild, uint dwEventThread, uint dwmsEventTime);

        [DllImport("user32.dll", SetLastError = true)]
        private static extern IntPtr SetWinEventHook(int eventMin, int eventMax,
            IntPtr hmodWinEventProc, WinEventProc lpfnWinEventProc, int idProcess, int idThread, int dwflags);

        [DllImport("user32.dll", SetLastError = true)]
        private static extern int UnhookWinEvent(IntPtr hWinEventHook);
    }
}
