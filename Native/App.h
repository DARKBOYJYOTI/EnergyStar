#pragma once
#include <windows.h>

enum class ServiceState {
    NotInstalled,
    Stopped,
    Running,
    Starting,
    Stopping
};

int RunUiProcess(HINSTANCE instance);
int RunAgentProcess();
int RunServiceProcess(const wchar_t* commandLine);

bool InstallEnergyStarService();
bool RemoveEnergyStarService();
bool StartEnergyStarService();
bool StopEnergyStarService();
ServiceState QueryEnergyStarServiceState();

