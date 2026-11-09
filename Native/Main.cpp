#include <windows.h>
#include <shellapi.h>
#include "App.h"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int exitCode = 0;
    bool handled = false;

    if (argv && argc > 1) {
        for (int i = 1; i < argc; ++i) {
            const wchar_t* arg = argv[i];
            if (_wcsicmp(arg, L"--service") == 0 || _wcsicmp(arg, L"-service") == 0 || _wcsicmp(arg, L"/service") == 0) {
                exitCode = RunServiceProcess(nullptr);
                handled = true;
                break;
            }
            if (_wcsicmp(arg, L"--agent") == 0 || _wcsicmp(arg, L"-agent") == 0 || _wcsicmp(arg, L"/agent") == 0) {
                exitCode = RunAgentProcess();
                handled = true;
                break;
            }
            if (_wcsicmp(arg, L"--install") == 0 || _wcsicmp(arg, L"-install") == 0 || _wcsicmp(arg, L"/install") == 0 || _wcsicmp(arg, L"-i") == 0) {
                exitCode = InstallEnergyStarService() ? 0 : 1;
                handled = true;
                break;
            }
            if (_wcsicmp(arg, L"--uninstall") == 0 || _wcsicmp(arg, L"-uninstall") == 0 || _wcsicmp(arg, L"/uninstall") == 0 ||
                _wcsicmp(arg, L"--remove") == 0 || _wcsicmp(arg, L"-remove") == 0 || _wcsicmp(arg, L"/remove") == 0 || _wcsicmp(arg, L"-u") == 0) {
                exitCode = RemoveEnergyStarService() ? 0 : 1;
                handled = true;
                break;
            }
            if (_wcsicmp(arg, L"--start") == 0 || _wcsicmp(arg, L"-start") == 0 || _wcsicmp(arg, L"/start") == 0) {
                exitCode = StartEnergyStarService() ? 0 : 1;
                handled = true;
                break;
            }
            if (_wcsicmp(arg, L"--stop") == 0 || _wcsicmp(arg, L"-stop") == 0 || _wcsicmp(arg, L"/stop") == 0) {
                exitCode = StopEnergyStarService() ? 0 : 1;
                handled = true;
                break;
            }
        }
        LocalFree(argv);
    } else if (argv) {
        LocalFree(argv);
    }

    if (handled) return exitCode;
    return RunUiProcess(instance);
}
