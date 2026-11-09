using System.IO;
using System.Text.Json;
using EnergyStar;

namespace EnergyStar.UI
{
    public sealed class EnergyStarSettings
    {
        public EnergyMode Mode { get; set; } = EnergyMode.Auto;
        public string Theme { get; set; } = "System";
        public bool LoggingEnabled { get; set; }
        public List<string> ExcludedExecutables { get; set; } = new();
    }

    internal static class SettingsStore
    {
        private static readonly string SettingsPath = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "EnergyStar", "settings.json");

        public static EnergyStarSettings Load()
        {
            try
            {
                if (File.Exists(SettingsPath))
                    return JsonSerializer.Deserialize<EnergyStarSettings>(File.ReadAllText(SettingsPath)) ?? new EnergyStarSettings();
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
