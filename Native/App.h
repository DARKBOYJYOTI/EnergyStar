#pragma once
#include <windows.h>

int RunUiProcess(HINSTANCE instance);
int RunAgentProcess();
int RunServiceProcess(const wchar_t* commandLine);
