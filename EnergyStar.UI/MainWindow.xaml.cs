using System.Collections.ObjectModel;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using EnergyStar;
using OpenFileDialog = Microsoft.Win32.OpenFileDialog;
using Button = System.Windows.Controls.Button;
using CheckBox = System.Windows.Controls.CheckBox;
using ComboBox = System.Windows.Controls.ComboBox;
using ComboBoxItem = System.Windows.Controls.ComboBoxItem;
using ListBox = System.Windows.Controls.ListBox;
using MessageBox = System.Windows.MessageBox;
using Brushes = System.Windows.Media.Brushes;
using Orientation = System.Windows.Controls.Orientation;
using HorizontalAlignment = System.Windows.HorizontalAlignment;

namespace EnergyStar.UI
{
    public partial class MainWindow : Window
    {
        private readonly EnergyStarSettings settings;
        private readonly Action<EnergyStarSettings> saveSettings;
        private readonly Action<string> applyTheme;
        private readonly ObservableCollection<string> exclusions;
        private readonly System.Windows.Threading.DispatcherTimer statusTimer;
        private TextBlock? statusLabel;
        private TextBlock? powerLabel;
        private TextBlock? managedLabel;
        private Border? statusDot;
        private string currentPage = "Overview";

        public MainWindow(EnergyStarSettings settings, Action<EnergyStarSettings> saveSettings,
            Action<string> applyTheme)
        {
            InitializeComponent();
            this.settings = settings;
            this.saveSettings = saveSettings;
            this.applyTheme = applyTheme;
            exclusions = new ObservableCollection<string>(settings.ExcludedExecutables);
            statusTimer = new System.Windows.Threading.DispatcherTimer { Interval = TimeSpan.FromSeconds(2) };
            statusTimer.Tick += (_, _) => RefreshStatus();
            statusTimer.Start();
            ShowPage("Overview");
        }

        private void Navigate_Click(object sender, RoutedEventArgs e)
        {
            if (sender is Button { Tag: string page }) ShowPage(page);
        }

        private void ShowPage(string page)
        {
            currentPage = page;
            PageTitle.Text = page;
            PageContent.Children.Clear();
            switch (page)
            {
                case "Overview": BuildOverview(); break;
                case "Exclusions": BuildExclusions(); break;
                case "Startup": BuildStartup(); break;
                case "Settings": BuildSettings(); break;
                case "Help": BuildHelp(); break;
                case "About": BuildAbout(); break;
                default: BuildOverview(); break;
            }
        }

        private void BuildOverview()
        {
            var status = GetStatus();
            AddText("Smarter power, automatically.", 27, FontWeights.Bold, "TextPrimary", 0, 5);
            AddText("Choose how EnergyStar manages inactive applications.", 14, FontWeights.Normal, "TextSecondary", 0, 22);

            var statusCard = Card();
            var panel = new StackPanel();
            panel.Children.Add(AddText("CURRENT STATUS", 10, FontWeights.Bold, "TextSecondary", 0, 12));
            var state = new StackPanel { Orientation = Orientation.Horizontal };
            statusDot = new Border { Width = 11, Height = 11, CornerRadius = new CornerRadius(6), Background = Brush(status.IsEnabled ? "Success" : "TextSecondary"), Margin = new Thickness(0, 5, 10, 0) };
            state.Children.Add(statusDot);
            statusLabel = AddText(status.IsEnabled ? "Energy management is active" : "Energy management is paused", 19, FontWeights.SemiBold, "TextPrimary", 0, 0);
            state.Children.Add(statusLabel);
            panel.Children.Add(state);
            powerLabel = AddText(status.OnBattery ? "Running on battery" : "Connected to power", 13, FontWeights.Normal, "TextSecondary", 21, 7);
            panel.Children.Add(powerLabel);
            managedLabel = AddText($"{status.ManagedProcessCount} processes managed this session", 12, FontWeights.Normal, "TextSecondary", 0, 1);
            panel.Children.Add(managedLabel);
            statusCard.Child = panel;
            PageContent.Children.Add(statusCard);

            var modeCard = Card();
            var modePanel = new StackPanel();
            modePanel.Children.Add(AddText("POWER BEHAVIOR", 10, FontWeights.Bold, "TextSecondary", 0, 10));
            modePanel.Children.Add(AddText("Choose when throttling is active", 16, FontWeights.SemiBold, "TextPrimary", 0, 4));
            modePanel.Children.Add(AddText("Auto saves power on battery. Always on forces EnergyStar on, even while charging.", 12, FontWeights.Normal, "TextSecondary", 0, 12));
            var modeBox = new ComboBox { Width = 260, HorizontalAlignment = HorizontalAlignment.Left, Padding = new Thickness(9), FontSize = 13 };
            modeBox.Items.Add(new ComboBoxItem { Content = "Auto — battery only", Tag = EnergyMode.Auto });
            modeBox.Items.Add(new ComboBoxItem { Content = "Always on — force enable", Tag = EnergyMode.AlwaysOn });
            modeBox.Items.Add(new ComboBoxItem { Content = "Off — disable throttling", Tag = EnergyMode.Off });
            modeBox.SelectedItem = modeBox.Items.Cast<ComboBoxItem>().First(item => (EnergyMode)item.Tag == settings.Mode);
            modeBox.SelectionChanged += (_, _) =>
            {
                if (modeBox.SelectedItem is ComboBoxItem selected && selected.Tag is EnergyMode value && settings.Mode != value)
                {
                    settings.Mode = value;
                    Save();
                }
            };
            modePanel.Children.Add(modeBox);
            modeCard.Child = modePanel;
            PageContent.Children.Add(modeCard);

            var tipCard = Card();
            tipCard.Child = new StackPanel
            {
                Children =
                {
                    AddText("QUICK TIP", 10, FontWeights.Bold, "Accent", 0, 8),
                    AddText("Add apps to Exclusions if EnergyStar should never throttle them.", 14, FontWeights.Medium, "TextPrimary", 0, 0)
                }
            };
            PageContent.Children.Add(tipCard);
        }

        private EnergyStatus GetStatus()
        {
            try { return EngineIpc.GetStatus(); }
            catch
            {
                return new EnergyStatus { Mode = settings.Mode, OnBattery = false, IsEnabled = false, ManagedProcessCount = 0 };
            }
        }

        private void RefreshStatus()
        {
            if (currentPage != "Overview" || statusLabel is null || powerLabel is null || managedLabel is null || statusDot is null)
                return;
            try
            {
                var status = EngineIpc.GetStatus();
                statusLabel.Text = status.IsEnabled ? "Energy management is active" : "Energy management is paused";
                statusDot.Background = Brush(status.IsEnabled ? "Success" : "TextSecondary");
                powerLabel.Text = status.OnBattery ? "Running on battery" : "Connected to power";
                managedLabel.Text = $"{status.ManagedProcessCount} processes managed this session";
            }
            catch
            {
                statusLabel.Text = "EnergyStar.Core.exe is not responding";
                statusDot.Background = Brush("Warning");
                powerLabel.Text = "Start EnergyStar again to reconnect";
                managedLabel.Text = "Process status unavailable";
            }
        }

        private void BuildExclusions()
        {
            AddText("Keep selected apps untouched.", 25, FontWeights.Bold, "TextPrimary", 0, 5);
            AddText("Excluded apps keep their normal scheduling behavior in every power mode.", 13, FontWeights.Normal, "TextSecondary", 0, 18);
            var card = Card();
            var panel = new StackPanel();
            var toolbar = new DockPanel { LastChildFill = false, Margin = new Thickness(0, 0, 0, 14) };
            var add = ActionButton("Browse for app");
            add.Click += AddExclusion_Click;
            DockPanel.SetDock(add, Dock.Right);
            toolbar.Children.Add(add);
            toolbar.Children.Add(AddText("EXCLUDED APPLICATIONS", 10, FontWeights.Bold, "TextSecondary", 0, 0));
            if (toolbar.Children[1] is FrameworkElement label)
                label.VerticalAlignment = VerticalAlignment.Center;
            panel.Children.Add(toolbar);

            var list = new ListBox { ItemsSource = exclusions, MinHeight = 190, BorderThickness = new Thickness(0), Background = Brushes.Transparent, Foreground = Brush("TextPrimary") };
            panel.Children.Add(list);

            var remove = ActionButton("Remove selected");
            remove.Margin = new Thickness(0, 14, 0, 0);
            remove.Click += (_, _) =>
            {
                if (list.SelectedItem is string path) { exclusions.Remove(path); PersistExclusions(); }
            };
            panel.Children.Add(remove);
            card.Child = panel;
            PageContent.Children.Add(card);
            PageContent.Children.Add(InfoCard("How it works", "Use Browse for app to select an .exe with File Explorer. EnergyStar stores its full path, so another app with the same file name is not excluded."));
        }

        private void AddExclusion_Click(object sender, RoutedEventArgs e)
        {
            var dialog = new OpenFileDialog { Title = "Choose application to exclude", Filter = "Applications (*.exe)|*.exe", CheckFileExists = true };
            if (dialog.ShowDialog(this) != true) return;
            var path = Path.GetFullPath(dialog.FileName);
            if (!exclusions.Contains(path, StringComparer.OrdinalIgnoreCase))
            {
                exclusions.Add(path);
                PersistExclusions();
            }
        }

        private void BuildStartup()
        {
            AddText("Start with Windows.", 25, FontWeights.Bold, "TextPrimary", 0, 5);
            AddText("Run EnergyStar at your sign-in with the permissions needed to manage app processes.", 13, FontWeights.Normal, "TextSecondary", 0, 18);
            var registered = StartupTaskManager.IsRegistered();
            PageContent.Children.Add(InfoCard(registered ? "Startup task is enabled" : "Startup task is disabled",
                $"Task Scheduler uses {StartupTaskManager.TaskXmlPath}. Enabling or disabling startup asks Windows for administrator approval."));
            var button = ActionButton(registered ? "Disable startup task" : "Enable startup task (administrator approval)");
            button.Margin = new Thickness(0, 3, 0, 18);
            button.Click += (_, _) =>
            {
                try
                {
                    if (registered) StartupTaskManager.RequestUnregister();
                    else StartupTaskManager.RequestRegister();
                    MessageBox.Show(this, "Approve the Windows prompt. Startup status will refresh when you reopen this page.", "EnergyStar", MessageBoxButton.OK, MessageBoxImage.Information);
                }
                catch (System.ComponentModel.Win32Exception ex)
                {
                    MessageBox.Show(this, ex.Message, "EnergyStar", MessageBoxButton.OK, MessageBoxImage.Warning);
                }
            };
            PageContent.Children.Add(button);
            PageContent.Children.Add(InfoCard("About elevated startup", "The desktop app itself runs without elevation. The scheduled task requests highest available privileges because Windows may deny access to protected processes."));
        }

        private void BuildSettings()
        {
            AddText("Make EnergyStar yours.", 25, FontWeights.Bold, "TextPrimary", 0, 5);
            AddText("Appearance and diagnostics", 13, FontWeights.Normal, "TextSecondary", 0, 18);

            var themeCard = Card();
            var themePanel = new StackPanel();
            themePanel.Children.Add(AddText("APPEARANCE", 10, FontWeights.Bold, "TextSecondary", 0, 8));
            themePanel.Children.Add(AddText("Color theme", 15, FontWeights.SemiBold, "TextPrimary", 0, 10));
            var themeBox = new ComboBox { Width = 220, HorizontalAlignment = HorizontalAlignment.Left, Padding = new Thickness(9) };
            foreach (var value in new[] { "System", "Light", "Dark" }) themeBox.Items.Add(value);
            themeBox.SelectedItem = settings.Theme;
            themeBox.SelectionChanged += (_, _) => { if (themeBox.SelectedItem is string value) { settings.Theme = value; Save(); applyTheme(value); } };
            themePanel.Children.Add(themeBox);
            themeCard.Child = themePanel;
            PageContent.Children.Add(themeCard);

            var logCard = Card();
            var logPanel = new StackPanel();
            logPanel.Children.Add(AddText("DIAGNOSTICS", 10, FontWeights.Bold, "TextSecondary", 0, 8));
            logPanel.Children.Add(AddText("Write debug log", 15, FontWeights.SemiBold, "TextPrimary", 0, 3));
            logPanel.Children.Add(AddText($"Write timestamped INFO, WARN, and ERROR messages to {Path.Combine(AppContext.BaseDirectory, "EnergyStar.log")}. Logging is off by default.", 12, FontWeights.Normal, "TextSecondary", 0, 10));
            var logCheck = new CheckBox { Content = "Enable EnergyStar.log", IsChecked = settings.LoggingEnabled, FontSize = 13, Foreground = Brush("TextPrimary") };
            logCheck.Checked += (_, _) => SetLogging(true);
            logCheck.Unchecked += (_, _) => SetLogging(false);
            logPanel.Children.Add(logCheck);
            logCard.Child = logPanel;
            PageContent.Children.Add(logCard);
        }

        private void BuildHelp()
        {
            AddText("Help & usage", 25, FontWeights.Bold, "TextPrimary", 0, 5);
            AddText("A quick guide to EnergyStar controls.", 13, FontWeights.Normal, "TextSecondary", 0, 18);
            PageContent.Children.Add(InfoCard("Auto mode", "EcoQoS and low priority apply while the PC runs on battery. Plug in power and EnergyStar restores processes it changed."));
            PageContent.Children.Add(InfoCard("Always on / Force enable", "Keeps background throttling active on battery and while connected to AC power."));
            PageContent.Children.Add(InfoCard("Off", "Stops throttling and restores tracked processes to their original priority class."));
            PageContent.Children.Add(InfoCard("Exclusions", "Browse for an executable to exclude it by full path. Excluded applications are never throttled by EnergyStar."));
            PageContent.Children.Add(InfoCard("Startup", $"Use Startup to create a logon task. Windows asks for administrator approval; the task definition is stored at {StartupTaskManager.TaskXmlPath}."));
            PageContent.Children.Add(InfoCard("Logging", "Enable logging in Settings to write diagnostics beside EnergyStar.Core.exe. Normal mode writes no log."));
            PageContent.Children.Add(InfoCard("Tray icon", "Closing this window hides EnergyStar in the notification area. Right-click its tray icon and choose Exit to stop it."));
            PageContent.Children.Add(InfoCard("Important", "Windows does not provide a supported API to read another app's EcoQoS state. EnergyStar may override an app's own EcoQoS choice; use Exclusions for apps that manage power themselves."));
        }

        private void BuildAbout()
        {
            AddText("About EnergyStar", 25, FontWeights.Bold, "TextPrimary", 0, 5);
            AddText("A lightweight Windows power companion.", 13, FontWeights.Normal, "TextSecondary", 0, 18);
            PageContent.Children.Add(InfoCard("Project", "EnergyStar uses Windows EcoQoS to reduce CPU impact for inactive applications."));
            PageContent.Children.Add(InfoCard("Original author", "This community fork builds on imbushuo/EnergyStar by Bingxing Wang. Original project: https://github.com/imbushuo/EnergyStar"));
            PageContent.Children.Add(InfoCard("License", "MIT-style license with Microsoft-specific terms. Read the included LICENSE file before redistributing."));
            PageContent.Children.Add(InfoCard("Build", ".NET 10 desktop application. Setup executables and portable ZIPs are provided separately."));
        }

        private void SetLogging(bool enabled)
        {
            settings.LoggingEnabled = enabled;
            Save();
        }

        private void PersistExclusions()
        {
            settings.ExcludedExecutables = exclusions.ToList();
            Save();
        }

        private void Save() => saveSettings(settings);

        private Border Card() => new() { Style = (Style)FindResource("Card") };

        private Border InfoCard(string title, string description)
        {
            var card = Card();
            var panel = new StackPanel();
            panel.Children.Add(AddText(title, 15, FontWeights.SemiBold, "TextPrimary", 0, 6));
            panel.Children.Add(AddText(description, 12, FontWeights.Normal, "TextSecondary", 0, 0));
            card.Child = panel;
            return card;
        }

        private TextBlock AddText(string text, double size, FontWeight weight, string brush, double left, double bottom) => new()
        {
            Text = text,
            FontSize = size,
            FontWeight = weight,
            Foreground = Brush(brush),
            Margin = new Thickness(left, 0, 0, bottom),
            TextWrapping = TextWrapping.Wrap
        };

        private SolidColorBrush Brush(string key) => (SolidColorBrush)FindResource(key);

        private Button ActionButton(string label) => new() { Content = label, HorizontalAlignment = HorizontalAlignment.Left };

        private void TitleBar_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
        {
            if (e.ButtonState == MouseButtonState.Pressed) DragMove();
        }

        private void Minimize_Click(object sender, RoutedEventArgs e) => WindowState = WindowState.Minimized;

        private void Close_Click(object sender, RoutedEventArgs e) => Hide();

        protected override void OnClosed(EventArgs e)
        {
            statusTimer.Stop();
            base.OnClosed(e);
        }
    }
}
