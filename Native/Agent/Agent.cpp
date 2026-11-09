#include <windows.h>
#include <sddl.h>
#include <string>
#include "../Shared/Protocol.h"

namespace {
struct ChildWindowProcess { DWORD hostPid{}; DWORD appPid{}; };

BOOL CALLBACK FindChildProcess(HWND window, LPARAM parameter) {
    auto* result = reinterpret_cast<ChildWindowProcess*>(parameter);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid && pid != result->hostPid) {
        result->appPid = pid;
        return FALSE;
    }
    return TRUE;
}

DWORD GetForegroundProcess(HWND window) {
    DWORD pid = 0;
    if (!window) return 0;
    GetWindowThreadProcessId(window, &pid);
    if (!pid) return 0;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return pid;
    wchar_t image[MAX_PATH]{};
    DWORD length = MAX_PATH;
    bool frameHost = QueryFullProcessImageNameW(process, 0, image, &length) &&
        _wcsicmp(wcsrchr(image, L'\\') ? wcsrchr(image, L'\\') + 1 : image, L"ApplicationFrameHost.exe") == 0;
    CloseHandle(process);
    if (frameHost) {
        ChildWindowProcess child{ pid, 0 };
        EnumChildWindows(window, FindChildProcess, reinterpret_cast<LPARAM>(&child));
        if (child.appPid) pid = child.appPid;
    }
    return pid;
}

DWORD SendForeground(DWORD pid) {
    HANDLE pipe = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PIPE_BUSY && WaitNamedPipeW(kPipeName, 250))
        pipe = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
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
    DWORD pid = GetForegroundProcess(window);
    if (pid) SendForeground(pid);
}
}

int RunAgentProcess() {
    DWORD sessionId = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &sessionId);
    const std::wstring mutexName = L"Global\\EnergyStar.Agent." + std::to_wstring(sessionId);
    SECURITY_ATTRIBUTES attributes{ sizeof(attributes), nullptr, FALSE };
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)(A;;GA;;;OW)",
            SDDL_REVISION_1, &descriptor, nullptr)) attributes.lpSecurityDescriptor = descriptor;
    HANDLE mutex = CreateMutexW(attributes.lpSecurityDescriptor ? &attributes : nullptr, TRUE, mutexName.c_str());
    if (descriptor) LocalFree(descriptor);
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) { if (mutex) CloseHandle(mutex); return 0; }
    HWINEVENTHOOK hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
        ForegroundChanged, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (!hook) { CloseHandle(mutex); return 1; }
    HWND foreground = GetForegroundWindow();
    DWORD initialPid = GetForegroundProcess(foreground);
    if (initialPid) SendForeground(initialPid);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    UnhookWinEvent(hook);
    CloseHandle(mutex);
    return 0;
}
