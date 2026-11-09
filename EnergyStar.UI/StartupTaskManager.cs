using System.Diagnostics;
using System.IO;
using System.Security.Principal;
using System.Xml.Linq;

namespace EnergyStar.UI
{
    internal static class StartupTaskManager
    {
        public const string TaskName = "\\EnergyStar";
        public static string TaskXmlPath => Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "EnergyStar", "EnergyStar.xml");

        public static bool IsRegistered()
        {
            using var process = Process.Start(new ProcessStartInfo(GetSchtasksPath())
            {
                Arguments = $"/Query /TN \"{TaskName}\"",
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true
            });
            process?.WaitForExit();
            return process?.ExitCode == 0;
        }

        public static void RequestRegister()
        {
            var userSid = WindowsIdentity.GetCurrent().User?.Value
                ?? throw new InvalidOperationException("Could not determine current user SID.");
            RequestElevated($"--register-startup --user-sid \"{userSid}\"");
        }

        public static void RequestUnregister() => RequestElevated("--unregister-startup");

        private static void RequestElevated(string argument)
        {
            var executable = Environment.ProcessPath
                ?? throw new InvalidOperationException("Could not find EnergyStar.exe path.");
            Process.Start(new ProcessStartInfo(executable, argument)
            {
                UseShellExecute = true,
                Verb = "runas"
            });
        }

        public static void Register(string userSid)
        {
            _ = new SecurityIdentifier(userSid);
            Directory.CreateDirectory(Path.GetDirectoryName(TaskXmlPath)!);
            var executable = Path.Combine(AppContext.BaseDirectory, "EnergyStar.Core.exe");
            if (!File.Exists(executable))
                throw new FileNotFoundException("EnergyStar.Core.exe is missing beside EnergyStar.exe.", executable);
            XNamespace ns = "http://schemas.microsoft.com/windows/2004/02/mit/task";
            var task = new XDocument(new XElement(ns + "Task",
                new XAttribute("version", "1.4"),
                new XElement(ns + "RegistrationInfo",
                    new XElement(ns + "Description", "Start EnergyStar at user logon with elevated process-management permissions."),
                    new XElement(ns + "Author", userSid)),
                new XElement(ns + "Triggers",
                    new XElement(ns + "LogonTrigger",
                        new XElement(ns + "Enabled", "true"),
                        new XElement(ns + "UserId", userSid))),
                new XElement(ns + "Principals",
                    new XElement(ns + "Principal",
                        new XAttribute("id", "EnergyStarUser"),
                        new XElement(ns + "UserId", userSid),
                        new XElement(ns + "LogonType", "InteractiveToken"),
                        new XElement(ns + "RunLevel", "HighestAvailable"))),
                new XElement(ns + "Settings",
                    new XElement(ns + "MultipleInstancesPolicy", "IgnoreNew"),
                    new XElement(ns + "StartWhenAvailable", "true"),
                    new XElement(ns + "DisallowStartIfOnBatteries", "false"),
                    new XElement(ns + "StopIfGoingOnBatteries", "false"),
                    new XElement(ns + "ExecutionTimeLimit", "PT0S"),
                    new XElement(ns + "Enabled", "true"),
                    new XElement(ns + "Hidden", "true"),
                    new XElement(ns + "Priority", "7")),
                new XElement(ns + "Actions",
                    new XAttribute("Context", "EnergyStarUser"),
                    new XElement(ns + "Exec",
                        new XElement(ns + "Command", executable),
                    new XElement(ns + "Arguments"),
                        new XElement(ns + "WorkingDirectory", Path.GetDirectoryName(executable))))));
            task.Save(TaskXmlPath);
            RunSchtasks($"/Create /TN \"{TaskName}\" /XML \"{TaskXmlPath}\" /F");
        }

        public static void Unregister()
        {
            RunSchtasks($"/Delete /TN \"{TaskName}\" /F");
        }

        private static void RunSchtasks(string arguments)
        {
            using var process = Process.Start(new ProcessStartInfo(GetSchtasksPath())
            {
                Arguments = arguments,
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true
            }) ?? throw new InvalidOperationException("Could not start Task Scheduler command.");
            var output = process.StandardOutput.ReadToEnd();
            var error = process.StandardError.ReadToEnd();
            process.WaitForExit();
            if (process.ExitCode != 0)
                throw new InvalidOperationException(string.IsNullOrWhiteSpace(error) ? output : error);
        }

        private static string GetSchtasksPath() => Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.System), "schtasks.exe");
    }
}
