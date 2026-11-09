using System.IO;
using System.Text.Json;

namespace EnergyStar
{
    public sealed class EnergyStarSettings
    {
        public EnergyMode Mode { get; set; } = EnergyMode.Auto;
        public string Theme { get; set; } = "System";
        public bool LoggingEnabled { get; set; }
        public List<string> ExcludedExecutables { get; set; } = new();
    }

    public static class SettingsStore
    {
        private static readonly string SettingsPath = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "EnergyStar", "settings.json");

        public static EnergyStarSettings Load()
        {
            try
            {
                if (File.Exists(SettingsPath))
                {
                    var settings = JsonSerializer.Deserialize<EnergyStarSettings>(File.ReadAllText(SettingsPath));
                    if (settings is not null)
                    {
                        if (!Enum.IsDefined(settings.Mode)) settings.Mode = EnergyMode.Auto;
                        if (settings.Theme is not ("System" or "Light" or "Dark")) settings.Theme = "System";
                        settings.ExcludedExecutables ??= new List<string>();
                        settings.ExcludedExecutables = settings.ExcludedExecutables
                            .Where(path => !string.IsNullOrWhiteSpace(path))
                            .ToList();
                        return settings;
                    }
                }
            }
            catch (Exception ex) when (ex is IOException or JsonException or UnauthorizedAccessException) { }
            return new EnergyStarSettings();
        }

        public static void Save(EnergyStarSettings settings)
        {
            Directory.CreateDirectory(Path.GetDirectoryName(SettingsPath)!);
            File.WriteAllText(SettingsPath, JsonSerializer.Serialize(settings, new JsonSerializerOptions { WriteIndented = true }));
        }
    }
}
