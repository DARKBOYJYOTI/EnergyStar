#include <windows.h>
#include <string>
#include "../Shared/Protocol.h"

namespace {
DWORD SendForeground(DWORD pid) {
    HANDLE pipe = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return GetLastError();
    PipeMessage request{};
    request.action = PipeAction::Foreground;
    request.foregroundPid = pid;
    DWORD written = 0, read = 0;
    BOOL ok = WriteFile(pipe, &request, kHeaderSize, &written, nullptr) && written == kHeaderSize &&
        ReadFile(pipe, &request, kHeaderSize, &read, nullptr) && read == kHeaderSize;
    CloseHandle(pipe);
    return ok ? ERROR_SUCCESS : GetLastError();
}

void CALLBACK ForegroundChanged(HWINEVENTHOOK, DWORD, HWND window, LONG, LONG, DWORD, DWORD) {
    DWORD pid = 0;
    if (window) GetWindowThreadProcessId(window, &pid);
    if (pid) SendForeground(pid);
}
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    DWORD sessionId = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &sessionId);
    std::wstring mutexName = L"Global\\EnergyStar.Agent." + std::to_wstring(sessionId);
    HANDLE mutex = CreateMutexW(nullptr, TRUE, mutexName.c_str());
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) { if (mutex) CloseHandle(mutex); return 0; }
    HWINEVENTHOOK hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
        ForegroundChanged, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (!hook) { CloseHandle(mutex); return 1; }
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    UnhookWinEvent(hook);
    CloseHandle(mutex);
    return 0;
}
