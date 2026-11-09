using System.Drawing;
using System.Windows;
using EnergyStar;
using Forms = System.Windows.Forms;

namespace EnergyStar.UI
{
    public partial class App : System.Windows.Application
    {
        private Forms.NotifyIcon? trayIcon;
        private EnergyStarSettings settings = new();
        private bool backgroundMode;
        private bool debugOverride;

        protected override void OnStartup(StartupEventArgs e)
        {
            base.OnStartup(e);
            if (e.Args.Contains("--register-startup", StringComparer.OrdinalIgnoreCase))
            {
                var sidIndex = Array.FindIndex(e.Args, argument => argument.Equals("--user-sid", StringComparison.OrdinalIgnoreCase));
                var userSid = sidIndex >= 0 && sidIndex + 1 < e.Args.Length
                    ? e.Args[sidIndex + 1]
                    : System.Security.Principal.WindowsIdentity.GetCurrent().User?.Value
                        ?? throw new InvalidOperationException("Could not determine current user SID.");
                RunTaskMaintenance(() => StartupTaskManager.Register(userSid));
                Shutdown();
                return;
            }
            if (e.Args.Contains("--unregister-startup", StringComparer.OrdinalIgnoreCase))
            {
                RunTaskMaintenance(StartupTaskManager.Unregister);
                Shutdown();
                return;
            }

            backgroundMode = e.Args.Contains("--background", StringComparer.OrdinalIgnoreCase);
            try
            {
                settings = SettingsStore.Load();
                ApplyTheme(settings.Theme);
                debugOverride = e.Args.Contains("--debug", StringComparer.OrdinalIgnoreCase);
                EngineIpc.EnsureRunning(debugOverride);
                EngineIpc.Configure(settings, debugOverride);
                CreateTrayIcon();

                var mainWindow = new MainWindow(settings, SaveSettings, ApplyTheme);
                MainWindow = mainWindow;
                if (backgroundMode) mainWindow.Hide();
                else mainWindow.Show();
            }
            catch (Exception ex)
            {
                Forms.MessageBox.Show(ex.Message, "EnergyStar could not start", Forms.MessageBoxButtons.OK, Forms.MessageBoxIcon.Error);
                Shutdown();
            }
        }

        private void RunTaskMaintenance(Action action)
        {
            try
            {
                action();
                Forms.MessageBox.Show("Startup task updated.", "EnergyStar", Forms.MessageBoxButtons.OK, Forms.MessageBoxIcon.Information);
            }
            catch (Exception ex)
            {
                Forms.MessageBox.Show(ex.Message, "EnergyStar startup task", Forms.MessageBoxButtons.OK, Forms.MessageBoxIcon.Error);
            }
        }

        private void SaveSettings(EnergyStarSettings updated)
        {
            settings = updated;
            SettingsStore.Save(settings);
            EngineIpc.Configure(settings, debugOverride);
            ApplyTheme(settings.Theme);
        }

        private void ApplyTheme(string theme)
        {
            var dark = theme.Equals("Dark", StringComparison.OrdinalIgnoreCase) ||
                (theme.Equals("System", StringComparison.OrdinalIgnoreCase) &&
                 Microsoft.Win32.Registry.GetValue("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", "AppsUseLightTheme", 1) is int value && value == 0);
            var colors = dark
                ? new[] { "#171A21", "#20242D", "#1D2129", "#F1F3F6", "#A7B0C0", "#78A2FF", "#293752", "#333A47", "#65D69A", "#F5B760" }
                : new[] { "#F4F6FA", "#FFFFFF", "#F8F9FC", "#172033", "#697386", "#326CE5", "#E8EFFF", "#E7EAF0", "#178A56", "#B96B12" };
            var keys = new[] { "AppBackground", "Surface", "Sidebar", "TextPrimary", "TextSecondary", "Accent", "AccentSoft", "BorderBrush", "Success", "Warning" };
            for (var i = 0; i < keys.Length; i++)
                ((System.Windows.Media.SolidColorBrush)Resources[keys[i]]).Color =
                    (System.Windows.Media.Color)System.Windows.Media.ColorConverter.ConvertFromString(colors[i]);
        }

        private void CreateTrayIcon()
        {
            var menu = new Forms.ContextMenuStrip();
            menu.Items.Add("Open EnergyStar", null, (_, _) => ShowWindow());
            menu.Items.Add("Exit", null, (_, _) => Shutdown());
            trayIcon = new Forms.NotifyIcon
            {
                Icon = Icon.ExtractAssociatedIcon(Environment.ProcessPath!) ?? SystemIcons.Application,
                Text = "EnergyStar",
                ContextMenuStrip = menu,
                Visible = true
            };
            trayIcon.DoubleClick += (_, _) => ShowWindow();
        }

        private void ShowWindow()
        {
            Dispatcher.Invoke(() =>
            {
                if (MainWindow is null) return;
                MainWindow.Show();
                MainWindow.WindowState = WindowState.Normal;
                MainWindow.Activate();
            });
        }

        protected override void OnExit(ExitEventArgs e)
        {
            trayIcon?.Dispose();
            base.OnExit(e);
        }
    }
}
