using EnergyStar;
using System.Threading;

namespace EnergyStar.Core
{
    internal static class Program
    {
        [STAThread]
        private static async Task Main(string[] args)
        {
            using var mutex = new Mutex(true, "Local\\EnergyStar.Core", out var createdNew);
            if (!createdNew) return;

            try
            {
                var settings = SettingsStore.Load();
                var debugLogging = args.Contains("--debug", StringComparer.OrdinalIgnoreCase);
                EnergyManager.Start(settings.Mode, settings.ExcludedExecutables, settings.LoggingEnabled || debugLogging);
                await EngineIpc.RunServerAsync(CancellationToken.None);
            }
            catch (Exception ex)
            {
                Logger.Error("EnergyStar core failed.", ex);
            }
            finally
            {
                EnergyManager.Stop();
            }
        }
    }
}
