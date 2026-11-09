#include <windows.h>
#include <winsvc.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <tlhelp32.h>
#include <processthreadsapi.h>
#include <shlobj.h>
#include <sddl.h>
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <fstream>
#include <filesystem>
#include <atomic>
#include "../Shared/Protocol.h"

#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Wtsapi32.lib")
#pragma comment(lib, "Userenv.lib")

namespace {
SERVICE_STATUS_HANDLE g_statusHandle{};
SERVICE_STATUS g_status{};
HANDLE g_stopEvent{};
std::mutex g_lock;
PipeMessage g_config;
DWORD g_foregroundPid{};
std::atomic<DWORD> g_boostPid{};
std::unordered_map<DWORD, DWORD> g_managed;
std::unordered_map<DWORD, HANDLE> g_agents;
std::atomic<DWORD> g_managedCount{};
std::atomic<bool> g_configChanged{};
std::wstring g_dataDir;

void Log(const wchar_t* text) {
    std::lock_guard guard(g_lock);
    if (!g_config.logging) return;
    std::wofstream log(std::filesystem::path(g_dataDir) / L"EnergyStar.log", std::ios::app);
    if (log) log << GetTickCount64() << L" " << text << L"\n";
}

std::wstring GetDataDirectory() {
    PWSTR known = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_CREATE, nullptr, &known))) return {};
    std::wstring path = std::wstring(known) + L"\\EnergyStar";
    CoTaskMemFree(known);
    std::filesystem::create_directories(path);
    return path;
}

bool IsExcluded(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return true;
    wchar_t image[MAX_PATH]{};
    DWORD size = MAX_PATH;
    bool excluded = true;
    if (QueryFullProcessImageNameW(process, 0, image, &size)) {
        std::lock_guard guard(g_lock);
        excluded = false;
        for (DWORD i = 0; i < g_config.exclusionCount; ++i)
            if (_wcsicmp(image, g_config.exclusions[i]) == 0) excluded = true;
        const wchar_t* name = wcsrchr(image, L'\\');
        name = name ? name + 1 : image;
        static const wchar_t* bypass[] = { L"EnergyStar.exe", L"EnergyStar.Service.exe", L"EnergyStar.Agent.exe", L"explorer.exe", L"dwm.exe", L"taskmgr.exe", L"csrss.exe", L"smss.exe", L"svchost.exe" };
        for (auto item : bypass) if (_wcsicmp(name, item) == 0) excluded = true;
    }
    CloseHandle(process);
    return excluded;
}

void Restore(DWORD pid) {
    auto it = g_managed.find(pid);
    if (it == g_managed.end()) return;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_INFORMATION, FALSE, pid);
    if (process) {
        PROCESS_POWER_THROTTLING_STATE state{};
        state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        state.StateMask = 0;
        SetProcessInformation(process, ProcessPowerThrottling, &state, sizeof(state));
        SetPriorityClass(process, it->second);
        CloseHandle(process);
    }
    g_managed.erase(it);
    g_managedCount = static_cast<DWORD>(g_managed.size());
}

void Throttle(DWORD pid) {
    DWORD foreground = 0;
    { std::lock_guard guard(g_lock); foreground = g_foregroundPid; }
    if (!pid || pid == foreground || IsExcluded(pid)) return;
    if (g_managed.contains(pid)) return;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_INFORMATION, FALSE, pid);
    if (!process) return;
    DWORD original = GetPriorityClass(process);
    if (!original) { CloseHandle(process); return; }
    PROCESS_POWER_THROTTLING_STATE state{};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    state.StateMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    SetProcessInformation(process, ProcessPowerThrottling, &state, sizeof(state));
    if (SetPriorityClass(process, IDLE_PRIORITY_CLASS)) { g_managed[pid] = original; g_managedCount = static_cast<DWORD>(g_managed.size()); }
    CloseHandle(process);
}

void ScanProcesses() {
    DWORD session = WTSGetActiveConsoleSessionId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W entry{ sizeof(entry) };
    if (Process32FirstW(snapshot, &entry)) do {
        DWORD ownerSession = 0;
        if (ProcessIdToSessionId(entry.th32ProcessID, &ownerSession) && ownerSession == session)
            Throttle(entry.th32ProcessID);
    } while (Process32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    for (auto it = g_managed.begin(); it != g_managed.end();) {
        HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, it->first);
        if (!process) { it = g_managed.erase(it); continue; }
        CloseHandle(process);
        ++it;
    }
}

void LoadConfig() {
    std::lock_guard guard(g_lock);
    std::wifstream config(std::filesystem::path(g_dataDir) / L"EnergyStar.conf");
    std::wstring line;
    while (std::getline(config, line)) {
        if (line == L"mode=always") g_config.mode = PowerMode::AlwaysOn;
        else if (line == L"mode=off") g_config.mode = PowerMode::Off;
        else if (line == L"mode=auto") g_config.mode = PowerMode::Automatic;
        else if (line == L"log=on") g_config.logging = 1;
        else if (line == L"log=off") g_config.logging = 0;
    }
    g_config.exclusionCount = 0;
    std::wifstream whitelist(std::filesystem::path(g_dataDir) / L"whitelist.conf");
    while (std::getline(whitelist, line) && g_config.exclusionCount < kMaxExclusions) {
        wchar_t full[MAX_PATH]{};
        if (GetFullPathNameW(line.c_str(), MAX_PATH, full, nullptr) && wcsrchr(full, L'.'))
            wcscpy_s(g_config.exclusions[g_config.exclusionCount++], full);
    }
}

bool SaveConfig(const PipeMessage& input) {
    if (input.exclusionCount > kMaxExclusions || input.mode > PowerMode::Off) return false;
    for (DWORD i = 0; i < input.exclusionCount; ++i) {
        const wchar_t* end = static_cast<const wchar_t*>(wmemchr(input.exclusions[i], L'\0', kPathChars));
        if (!end || end == input.exclusions[i] || !wcsrchr(input.exclusions[i], L'\\')) return false;
        const wchar_t* extension = wcsrchr(input.exclusions[i], L'.');
        if (!extension || _wcsicmp(extension, L".exe") != 0) return false;
        wchar_t canonical[MAX_PATH]{};
        if (!GetFullPathNameW(input.exclusions[i], MAX_PATH, canonical, nullptr)) return false;
    }
    wchar_t configTemp[MAX_PATH]{}, listTemp[MAX_PATH]{}, configFile[MAX_PATH]{}, listFile[MAX_PATH]{};
    swprintf_s(configFile, L"%s\\EnergyStar.conf", g_dataDir.c_str());
    swprintf_s(listFile, L"%s\\whitelist.conf", g_dataDir.c_str());
    swprintf_s(configTemp, L"%s.tmp", configFile);
    swprintf_s(listTemp, L"%s.tmp", listFile);
    {
        std::wofstream out(configTemp);
        if (!out) return false;
        out << L"mode=" << (input.mode == PowerMode::Off ? L"off" : input.mode == PowerMode::AlwaysOn ? L"always" : L"auto") << L"\nlog=" << (input.logging ? L"on" : L"off") << L"\n";
    }
    {
        std::wofstream out(listTemp);
        if (!out) { DeleteFileW(configTemp); return false; }
        for (DWORD i = 0; i < input.exclusionCount; ++i) out << input.exclusions[i] << L"\n";
    }
    if (!MoveFileExW(configTemp, configFile, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ||
        !MoveFileExW(listTemp, listFile, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return false;
    LoadConfig();
    g_configChanged = true;
    return true;
}

DWORD WINAPI PipeThread(void*) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)", SDDL_REVISION_1, &descriptor, nullptr)) return 1;
    SECURITY_ATTRIBUTES attributes{ sizeof(attributes), descriptor, FALSE };
    while (WaitForSingleObject(g_stopEvent, 0) != WAIT_OBJECT_0) {
        HANDLE pipe = CreateNamedPipeW(kPipeName, PIPE_ACCESS_DUPLEX, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1, sizeof(PipeMessage), sizeof(PipeMessage), 1000, descriptor ? &attributes : nullptr);
        if (pipe == INVALID_HANDLE_VALUE) { Sleep(1000); continue; }
        BOOL connected = ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED;
        if (connected) {
            PipeMessage request{}, response{};
            DWORD bytes = 0;
            BOOL readOk = ReadFile(pipe, &request, kHeaderSize, &bytes, nullptr);
            bool valid = (readOk || GetLastError() == ERROR_MORE_DATA) && bytes == kHeaderSize && request.magic == kProtocolMagic;
            if (valid && request.action == PipeAction::Configure) {
                DWORD remaining = sizeof(request) - kHeaderSize, received = 0;
                valid = ReadFile(pipe, reinterpret_cast<BYTE*>(&request) + kHeaderSize, remaining, &received, nullptr) && received == remaining;
            }
            if (valid) {
                bool saved = true;
                if (request.action == PipeAction::Configure) saved = SaveConfig(request);
                else if (request.action == PipeAction::Foreground) {
                    { std::lock_guard guard(g_lock); g_foregroundPid = request.foregroundPid; }
                    g_boostPid = request.foregroundPid;
                }
                std::lock_guard guard(g_lock);
                response = g_config;
                SYSTEM_POWER_STATUS power{};
                GetSystemPowerStatus(&power);
                response.onBattery = power.ACLineStatus == 0;
                response.enabled = response.mode == PowerMode::AlwaysOn || (response.mode == PowerMode::Automatic && response.onBattery);
                response.processCount = g_managedCount.load();
                response.foregroundPid = saved ? 1 : 0;
                DWORD responseSize = request.action == PipeAction::Foreground ? kHeaderSize : sizeof(response);
                WriteFile(pipe, &response, responseSize, &bytes, nullptr);
            }
        }
        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
    if (descriptor) LocalFree(descriptor);
    return 0;
}

void LaunchAgents() {
    DWORD count = 0;
    PWTS_SESSION_INFOW sessions = nullptr;
    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count)) return;
    for (DWORD i = 0; i < count; ++i) {
        if (sessions[i].State != WTSActive) continue;
        auto old = g_agents.find(sessions[i].SessionId);
        if (old != g_agents.end() && WaitForSingleObject(old->second, 0) == WAIT_TIMEOUT) continue;
        if (old != g_agents.end()) { CloseHandle(old->second); g_agents.erase(old); }
        HANDLE token = nullptr, primary = nullptr;
        if (!WTSQueryUserToken(sessions[i].SessionId, &token)) continue;
        if (DuplicateTokenEx(token, MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenPrimary, &primary)) {
            wchar_t module[MAX_PATH]{}; GetModuleFileNameW(nullptr, module, MAX_PATH);
            std::wstring agent(module); auto slash = agent.find_last_of(L"\\/"); agent.resize(slash + 1); agent += L"EnergyStar.Agent.exe";
            STARTUPINFOW startup{ sizeof(startup) }; PROCESS_INFORMATION process{};
            LPVOID environment = nullptr;
            CreateEnvironmentBlock(&environment, primary, FALSE);
            if (CreateProcessAsUserW(primary, agent.c_str(), nullptr, nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, environment, nullptr, &startup, &process)) {
                CloseHandle(process.hThread); g_agents[sessions[i].SessionId] = process.hProcess;
            }
            if (environment) DestroyEnvironmentBlock(environment);
            CloseHandle(primary);
        }
        CloseHandle(token);
    }
    WTSFreeMemory(sessions);
}

void WINAPI ServiceControl(DWORD control) {
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
        g_status.dwCurrentState = SERVICE_STOP_PENDING; SetServiceStatus(g_statusHandle, &g_status); SetEvent(g_stopEvent);
    }
}

void WINAPI ServiceMain(DWORD, wchar_t**) {
    g_statusHandle = RegisterServiceCtrlHandlerW(L"EnergyStarService", ServiceControl);
    if (!g_statusHandle) return;
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = SERVICE_START_PENDING;
    SetServiceStatus(g_statusHandle, &g_status);
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_dataDir = GetDataDirectory();
    LoadConfig();
    LaunchAgents();
    HANDLE pipeThread = CreateThread(nullptr, 0, PipeThread, nullptr, 0, nullptr);
    g_status.dwCurrentState = SERVICE_RUNNING; SetServiceStatus(g_statusHandle, &g_status);
    while (WaitForSingleObject(g_stopEvent, 5000) == WAIT_TIMEOUT) {
        LaunchAgents();
        DWORD foreground = g_boostPid.exchange(0);
        if (foreground) Restore(foreground);
        SYSTEM_POWER_STATUS power{}; GetSystemPowerStatus(&power);
        PowerMode mode;
        { std::lock_guard guard(g_lock); mode = g_config.mode; }
        bool enabled = mode == PowerMode::AlwaysOn || (mode == PowerMode::Automatic && power.ACLineStatus == 0);
        if (g_configChanged.exchange(false)) {
            std::vector<DWORD> restore;
            for (const auto& item : g_managed) if (IsExcluded(item.first)) restore.push_back(item.first);
            for (DWORD pid : restore) Restore(pid);
        }
        if (enabled) ScanProcesses();
        else { std::vector<DWORD> pids; for (auto& item : g_managed) pids.push_back(item.first); for (DWORD pid : pids) Restore(pid); }
    }
    { std::vector<DWORD> pids; for (auto& item : g_managed) pids.push_back(item.first); for (DWORD pid : pids) Restore(pid); }
    for (auto& item : g_agents) { TerminateProcess(item.second, 0); CloseHandle(item.second); }
    g_agents.clear();
    if (pipeThread) { WaitForSingleObject(pipeThread, 2000); CloseHandle(pipeThread); }
    CloseHandle(g_stopEvent);
    g_status.dwCurrentState = SERVICE_STOPPED; SetServiceStatus(g_statusHandle, &g_status);
}

bool InstallService() {
    wchar_t path[MAX_PATH]{}; GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring binaryPath = L"\"" + std::wstring(path) + L"\"";
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!scm) return false;
    SC_HANDLE service = CreateServiceW(scm, L"EnergyStarService", L"EnergyStar Core", SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, binaryPath.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
    if (!service && GetLastError() == ERROR_SERVICE_EXISTS) service = OpenServiceW(scm, L"EnergyStarService", SERVICE_ALL_ACCESS);
    bool ok = service != nullptr;
    if (ok) {
        SC_ACTION actions[3] = { { SC_ACTION_RESTART, 5000 }, { SC_ACTION_RESTART, 15000 }, { SC_ACTION_RESTART, 60000 } };
        SERVICE_FAILURE_ACTIONSW failure{ 86400, nullptr, nullptr, 3, actions };
        ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS, &failure);
        SERVICE_FAILURE_ACTIONS_FLAG flag{ TRUE };
        ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &flag);
        StartServiceW(service, 0, nullptr);
        CloseServiceHandle(service);
    }
    CloseServiceHandle(scm); return ok;
}

void RemoveService() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!scm) return;
    SC_HANDLE service = OpenServiceW(scm, L"EnergyStarService", SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS);
    if (service) {
        SERVICE_STATUS status{};
        ControlService(service, SERVICE_CONTROL_STOP, &status);
        for (int attempt = 0; attempt < 50; ++attempt) {
            if (!QueryServiceStatus(service, &status) || status.dwCurrentState == SERVICE_STOPPED) break;
            Sleep(200);
        }
        DeleteService(service);
        CloseServiceHandle(service);
    }
    CloseServiceHandle(scm);
}
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR commandLine, int) {
    if (commandLine && _wcsicmp(commandLine, L"--install") == 0) return InstallService() ? 0 : 1;
    if (commandLine && _wcsicmp(commandLine, L"--uninstall") == 0) { RemoveService(); return 0; }
    SERVICE_TABLE_ENTRYW table[] = { { const_cast<LPWSTR>(L"EnergyStarService"), ServiceMain }, { nullptr, nullptr } };
    return StartServiceCtrlDispatcherW(table) ? 0 : 1;
}
