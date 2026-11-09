using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Security.Principal;
using System.Text.Json;

namespace EnergyStar
{
    public static class EngineIpc
    {
        private static string PipeName
        {
            get
            {
                using var identity = WindowsIdentity.GetCurrent();
                using var process = Process.GetCurrentProcess();
                return $"EnergyStar.Core.v1.{identity.User?.Value}.{process.SessionId}";
            }
        }

        public static void EnsureRunning(bool debugLogging)
        {
            if (!TrySend(new EngineRequest { Action = "status" }, out _))
            {
                var executable = Path.Combine(AppContext.BaseDirectory, "EnergyStar.Core.exe");
                if (!File.Exists(executable))
                    throw new FileNotFoundException("EnergyStar.Core.exe is missing beside EnergyStar.exe.", executable);

                var startInfo = new ProcessStartInfo(executable)
                {
                    UseShellExecute = false,
                    CreateNoWindow = true,
                    WorkingDirectory = AppContext.BaseDirectory
                };
                if (debugLogging) startInfo.ArgumentList.Add("--debug");
                Process.Start(startInfo);
            }

            for (var attempt = 0; attempt < 30; attempt++)
            {
                if (TrySend(new EngineRequest { Action = "status" }, out _)) return;
                Thread.Sleep(100);
            }
            throw new InvalidOperationException("EnergyStar.Core.exe did not start or accept a local connection.");
        }

        public static EnergyStatus GetStatus()
        {
            if (!TrySend(new EngineRequest { Action = "status" }, out var status) || status is null)
                throw new InvalidOperationException("EnergyStar engine is not responding.");
            return status;
        }

        public static void Configure(EnergyStarSettings settings, bool debugLogging)
        {
            EnsureRunning(debugLogging);
            var request = new EngineRequest
            {
                Action = "configure",
                Mode = settings.Mode,
                ExcludedExecutables = settings.ExcludedExecutables,
                LoggingEnabled = settings.LoggingEnabled || debugLogging
            };
            if (!TrySend(request, out _))
                throw new InvalidOperationException("Could not update EnergyStar engine settings.");
        }

        public static async Task RunServerAsync(CancellationToken cancellationToken)
        {
            while (!cancellationToken.IsCancellationRequested)
            {
                await using var pipe = new NamedPipeServerStream(PipeName, PipeDirection.InOut, 1,
                    PipeTransmissionMode.Byte, PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly);
                try
                {
                    await pipe.WaitForConnectionAsync(cancellationToken);
                    using var reader = new StreamReader(pipe, leaveOpen: true);
                    await using var writer = new StreamWriter(pipe, leaveOpen: true) { AutoFlush = true };
                    var line = await reader.ReadLineAsync(cancellationToken);
                    if (string.IsNullOrWhiteSpace(line)) continue;

                    try
                    {
                        var request = JsonSerializer.Deserialize<EngineRequest>(line)
                            ?? throw new InvalidDataException("Empty engine request.");
                        if (request.Action == "configure")
                            EnergyManager.Configure(request.Mode, request.ExcludedExecutables ?? new List<string>(), request.LoggingEnabled);
                        var response = JsonSerializer.Serialize(EnergyManager.GetStatus());
                        await writer.WriteLineAsync(response);
                    }
                    catch (Exception ex) when (ex is JsonException or InvalidDataException or ArgumentException)
                    {
                        Logger.Error("Rejected invalid engine IPC request.", ex);
                        await writer.WriteLineAsync("{}");
                    }
                }
                catch (IOException ex)
                {
                    Logger.Warning($"Engine IPC client disconnected: {ex.Message}");
                }
            }
        }

        private static bool TrySend(EngineRequest request, out EnergyStatus? status)
        {
            status = null;
            try
            {
                using var pipe = new NamedPipeClientStream(".", PipeName, PipeDirection.InOut, PipeOptions.None);
                pipe.Connect(250);
                using var writer = new StreamWriter(pipe, leaveOpen: true) { AutoFlush = true };
                writer.WriteLine(JsonSerializer.Serialize(request));
                using var reader = new StreamReader(pipe, leaveOpen: true);
                var response = reader.ReadLine();
                status = response is null ? null : JsonSerializer.Deserialize<EnergyStatus>(response);
                return status is not null;
            }
            catch (Exception ex) when (ex is IOException or TimeoutException or JsonException or UnauthorizedAccessException)
            {
                return false;
            }
        }

        private sealed class EngineRequest
        {
            public string Action { get; set; } = "status";
            public EnergyMode Mode { get; set; }
            public List<string>? ExcludedExecutables { get; set; }
            public bool LoggingEnabled { get; set; }
        }
    }
}
