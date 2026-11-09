using System.Text;

namespace EnergyStar
{
    public static class Logger
    {
        private static readonly object Sync = new();
        private static StreamWriter? writer;

        public static bool IsEnabled { get; private set; }

        public static void Initialize(string[] args)
        {
            Configure(args.Any(arg => arg.Equals("--debug", StringComparison.OrdinalIgnoreCase)));
        }

        public static void Configure(bool enabled)
        {
            if (IsEnabled == enabled && (enabled == (writer is not null))) return;

            Close();
            IsEnabled = enabled;
            if (!IsEnabled) return;

            try
            {
                var path = Path.Combine(AppContext.BaseDirectory, "EnergyStar.log");
                writer = new StreamWriter(new FileStream(path, FileMode.Append, FileAccess.Write, FileShare.ReadWrite), new UTF8Encoding(false))
                {
                    AutoFlush = true
                };
                Info($"Logging enabled. Log file: {path}");
            }
            catch (Exception ex)
            {
                IsEnabled = false;
                Console.Error.WriteLine($"Could not open debug log: {ex.Message}");
            }
        }

        public static void Info(string message) => Write("INFO", message);

        public static void Warning(string message) => Write("WARN", message);

        public static void Error(string message, Exception? exception = null) =>
            Write("ERROR", exception is null ? message : $"{message}{Environment.NewLine}{exception}");

        private static void Write(string level, string message)
        {
            if (!IsEnabled || writer is null) return;

            lock (Sync)
            {
                try
                {
                    writer.WriteLine($"{DateTimeOffset.Now:yyyy-MM-dd HH:mm:ss.fff zzz} [{level}] {message}");
                }
                catch (IOException) { }
                catch (ObjectDisposedException) { }
            }
        }

        public static void Close()
        {
            lock (Sync)
            {
                writer?.Dispose();
                writer = null;
            }
        }
    }
}
