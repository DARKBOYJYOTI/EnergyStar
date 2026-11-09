#include <windows.h>
#include "App.h"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int) {
    if (commandLine && _wcsicmp(commandLine, L"--service") == 0) return RunServiceProcess(nullptr);
    if (commandLine && _wcsicmp(commandLine, L"--install") == 0) return RunServiceProcess(commandLine);
    if (commandLine && _wcsicmp(commandLine, L"--uninstall") == 0) return RunServiceProcess(commandLine);
    if (commandLine && _wcsicmp(commandLine, L"--agent") == 0) return RunAgentProcess();
    return RunUiProcess(instance);
}
