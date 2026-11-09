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
#include <unordered_set>
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
HANDLE g_foregroundEvent{};
struct ManagedProcess { DWORD priority{}; FILETIME created{}; };
std::unordered_map<DWORD, ManagedProcess> g_managed;
std::unordered_map<DWORD, HANDLE> g_agents;
std::atomic<DWORD> g_managedCount{};
std::atomic<bool> g_configChanged{};
std::wstring g_dataDir;
std::unordered_map<DWORD, DWORD> g_foregroundPids;

void Log(const wchar_t* text) {
    std::lock_guard guard(g_lock);
    if (!g_config.logging) return;
    std::wofstream log(std::filesystem::path(g_dataDir) / L"EnergyStar.log", std::ios::app);
    if (log) log << GetTickCount64() << L" " << text << L"\n";
}

std::wstring GetDataDirectory() {
    wchar_t module[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, module, MAX_PATH)) return {};
    std::filesystem::path directory = std::filesystem::path(module).parent_path();
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    return directory.wstring();
}

void MigrateLegacySettings() {
    std::error_code error;
    if (std::filesystem::exists(std::filesystem::path(g_dataDir) / L"EnergyStar.conf", error)) return;
    PWSTR known = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &known))) return;
    const std::filesystem::path legacy = std::filesystem::path(known) / L"EnergyStar";
    CoTaskMemFree(known);
    for (const wchar_t* name : { L"EnergyStar.conf", L"whitelist.conf" }) {
        const std::filesystem::path source = legacy / name;
        if (std::filesystem::exists(source, error))
            std::filesystem::copy_file(source, std::filesystem::path(g_dataDir) / name,
                std::filesystem::copy_options::skip_existing, error);
    }
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
        static const wchar_t* bypass[] = { L"EnergyStar.exe", L"EnergyStar.Service.exe", L"EnergyStar.Agent.exe", L"explorer.exe", L"dwm.exe", L"taskmgr.exe", L"procmon.exe", L"procmon64.exe", L"msedge.exe", L"WebViewHost.exe", L"ApplicationFrameHost.exe", L"Widgets.exe", L"ShellExperienceHost.exe", L"StartMenuExperienceHost.exe", L"SearchHost.exe", L"sihost.exe", L"fontdrvhost.exe", L"csrss.exe", L"smss.exe", L"svchost.exe", L"WUDFRd.exe" };
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
        FILETIME created{}, exited{}, kernel{}, user{};
        if (!GetProcessTimes(process, &created, &exited, &kernel, &user) ||
            CompareFileTime(&created, &it->second.created) != 0) {
            CloseHandle(process);
            g_managed.erase(it);
            g_managedCount = static_cast<DWORD>(g_managed.size());
            return;
        }
        PROCESS_POWER_THROTTLING_STATE state{};
        state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        state.StateMask = 0;
        BOOL qosRestored = SetProcessInformation(process, ProcessPowerThrottling, &state, sizeof(state));
        BOOL priorityRestored = SetPriorityClass(process, it->second.priority);
        if (qosRestored || priorityRestored) Log(L"Restored process state.");
        CloseHandle(process);
    }
    g_managed.erase(it);
    g_managedCount = static_cast<DWORD>(g_managed.size());
}

void Throttle(DWORD pid) {
    if (!pid || IsExcluded(pid)) return;
    if (g_managed.contains(pid)) return;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_INFORMATION, FALSE, pid);
    if (!process) return;
    DWORD original = GetPriorityClass(process);
    if (!original) { CloseHandle(process); return; }
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) { CloseHandle(process); return; }
    PROCESS_POWER_THROTTLING_STATE state{};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    state.StateMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    BOOL qosApplied = SetProcessInformation(process, ProcessPowerThrottling, &state, sizeof(state));
    BOOL priorityApplied = SetPriorityClass(process, IDLE_PRIORITY_CLASS);
    if (priorityApplied || qosApplied) {
        g_managed[pid] = ManagedProcess{ original, created };
        g_managedCount = static_cast<DWORD>(g_managed.size());
        Log(L"Throttled background process.");
    } else {
        PROCESS_POWER_THROTTLING_STATE restore{};
        restore.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        restore.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        SetProcessInformation(process, ProcessPowerThrottling, &restore, sizeof(restore));
        if (priorityApplied) SetPriorityClass(process, original);
    }
    CloseHandle(process);
}

std::unordered_map<DWORD, std::vector<DWORD>> GetProcessTree() {
    std::unordered_map<DWORD, std::vector<DWORD>> children;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return children;
    PROCESSENTRY32W entry{ sizeof(entry) };
    if (Process32FirstW(snapshot, &entry)) do {
        children[entry.th32ParentProcessID].push_back(entry.th32ProcessID);
    } while (Process32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    return children;
}

std::vector<DWORD> GetDescendants(DWORD root, const std::unordered_map<DWORD, std::vector<DWORD>>& children) {
    std::vector<DWORD> result, pending{ root };
    for (size_t i = 0; i < pending.size(); ++i) {
        auto found = children.find(pending[i]);
        if (found == children.end()) continue;
        for (DWORD child : found->second) { result.push_back(child); pending.push_back(child); }
    }
    return result;
}

std::unordered_set<DWORD> GetActiveSessions() {
    std::unordered_set<DWORD> active;
    PWTS_SESSION_INFOW sessions = nullptr;
    DWORD count = 0;
    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count)) return active;
    for (DWORD i = 0; i < count; ++i)
        if (sessions[i].State == WTSActive) active.insert(sessions[i].SessionId);
    WTSFreeMemory(sessions);
    return active;
}

bool IsSessionActive(DWORD sessionId) {
    WTS_CONNECTSTATE_CLASS* state = nullptr;
    DWORD bytes = 0;
    const bool ok = WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, sessionId,
        WTSConnectState, reinterpret_cast<LPWSTR*>(&state), &bytes);
    const bool active = ok && state && *state == WTSActive;
    if (ok && state) WTSFreeMemory(state);
    return active;
}

void ScanProcesses() {
    const auto activeSessions = GetActiveSessions();
    if (activeSessions.empty()) return;
    auto tree = GetProcessTree();
    std::unordered_map<DWORD, DWORD> foreground;
    { std::lock_guard guard(g_lock); foreground = g_foregroundPids; }
    std::unordered_map<DWORD, std::unordered_set<DWORD>> foregroundTrees;
    for (const auto& [session, pid] : foreground) {
        if (!pid || !activeSessions.contains(session)) continue;
        auto& foregroundTree = foregroundTrees[session];
        foregroundTree.insert(pid);
        for (DWORD child : GetDescendants(pid, tree)) foregroundTree.insert(child);
    }
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W entry{ sizeof(entry) };
    if (Process32FirstW(snapshot, &entry)) do {
        DWORD ownerSession = 0;
        if (ProcessIdToSessionId(entry.th32ProcessID, &ownerSession) && activeSessions.contains(ownerSession)) {
            const auto found = foregroundTrees.find(ownerSession);
            if (found != foregroundTrees.end() && found->second.contains(entry.th32ProcessID)) Restore(entry.th32ProcessID);
            else Throttle(entry.th32ProcessID);
        }
    } while (Process32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    for (auto it = g_managed.begin(); it != g_managed.end();) {
        HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, it->first);
        if (!process) { it = g_managed.erase(it); g_managedCount = static_cast<DWORD>(g_managed.size()); continue; }
        FILETIME created{}, exited{}, kernel{}, user{};
        bool sameProcess = GetProcessTimes(process, &created, &exited, &kernel, &user) &&
            CompareFileTime(&created, &it->second.created) == 0;
        CloseHandle(process);
        if (!sameProcess) { it = g_managed.erase(it); g_managedCount = static_cast<DWORD>(g_managed.size()); continue; }
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
                    DWORD processSession = 0;
                    if (ProcessIdToSessionId(request.foregroundPid, &processSession) && IsSessionActive(processSession)) {
                        { std::lock_guard guard(g_lock); g_foregroundPids[processSession] = request.foregroundPid; }
                        SetEvent(g_foregroundEvent);
                        Log(L"Foreground process changed.");
                    }
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

bool AgentRunning(DWORD sessionId) {
    const std::wstring mutexName = L"Global\\EnergyStar.Agent." + std::to_wstring(sessionId);
    HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, mutexName.c_str());
    if (!mutex) return false;
    CloseHandle(mutex);
    return true;
}

void LaunchAgents() {
    for (DWORD sessionId : GetActiveSessions()) {
        if (AgentRunning(sessionId)) continue;
        auto old = g_agents.find(sessionId);
        if (old != g_agents.end()) {
            if (WaitForSingleObject(old->second, 0) == WAIT_TIMEOUT) continue;
            CloseHandle(old->second);
            g_agents.erase(old);
        }
        HANDLE token = nullptr, primary = nullptr;
        if (!WTSQueryUserToken(sessionId, &token)) continue;
        if (DuplicateTokenEx(token, MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenPrimary, &primary)) {
            wchar_t module[MAX_PATH]{}; GetModuleFileNameW(nullptr, module, MAX_PATH);
            std::wstring commandLine = L"\"" + std::wstring(module) + L"\" --agent";
            STARTUPINFOW startup{ sizeof(startup) }; PROCESS_INFORMATION process{};
            LPVOID environment = nullptr;
            CreateEnvironmentBlock(&environment, primary, FALSE);
            if (CreateProcessAsUserW(primary, module, commandLine.data(), nullptr, nullptr, FALSE,
                    CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, environment, nullptr, &startup, &process)) {
                CloseHandle(process.hThread); g_agents[sessionId] = process.hProcess;
            }
            if (environment) DestroyEnvironmentBlock(environment);
            CloseHandle(primary);
        }
        CloseHandle(token);
    }
}

void WINAPI ServiceControl(DWORD control) {
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
        g_status.dwControlsAccepted = 0;
        g_status.dwCurrentState = SERVICE_STOP_PENDING;
        SetServiceStatus(g_statusHandle, &g_status);
        SetEvent(g_stopEvent);
    }
}

void WINAPI ServiceMain(DWORD, wchar_t**) {
    g_statusHandle = RegisterServiceCtrlHandlerW(L"EnergyStarService", ServiceControl);
    if (!g_statusHandle) return;
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = SERVICE_START_PENDING;
    SetServiceStatus(g_statusHandle, &g_status);
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_foregroundEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_dataDir = GetDataDirectory();
    MigrateLegacySettings();
    LoadConfig();
    LaunchAgents();
    HANDLE pipeThread = CreateThread(nullptr, 0, PipeThread, nullptr, 0, nullptr);
    g_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    g_status.dwCurrentState = SERVICE_RUNNING; SetServiceStatus(g_statusHandle, &g_status);
    HANDLE events[] = { g_stopEvent, g_foregroundEvent };
    DWORD waitResult;
    do {
        waitResult = WaitForMultipleObjects(2, events, FALSE, 500);
        LaunchAgents();
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
    } while (waitResult != WAIT_OBJECT_0);
    { std::vector<DWORD> pids; for (auto& item : g_managed) pids.push_back(item.first); for (DWORD pid : pids) Restore(pid); }
    for (auto& item : g_agents) { TerminateProcess(item.second, 0); CloseHandle(item.second); }
    g_agents.clear();
    if (pipeThread) { WaitForSingleObject(pipeThread, 2000); CloseHandle(pipeThread); }
    CloseHandle(g_stopEvent);
    CloseHandle(g_foregroundEvent);
    g_status.dwCurrentState = SERVICE_STOPPED; SetServiceStatus(g_statusHandle, &g_status);
}

bool ClearFailureActions(SC_HANDLE service) {
    SC_ACTION action[1] = { { SC_ACTION_NONE, 0 } };
    SERVICE_FAILURE_ACTIONSW failure{ 0, nullptr, nullptr, 1, action };
    const bool actionsCleared = ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS, &failure) != FALSE;
    SERVICE_FAILURE_ACTIONS_FLAG flag{ FALSE };
    const bool flagCleared = ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &flag) != FALSE;
    return actionsCleared && flagCleared;
}

bool SetServicePermissions(SC_HANDLE service) {
    constexpr wchar_t kSddl[] = L"D:(A;;CCLCSWRPWPDTLOCRRC;;;SY)(A;;CCDCLCSWRPWPDTLOCRSDRCWDWO;;;BA)(A;;CCLCSWRPDTLOCRRC;;;IU)(A;;CCLCSWLOCRRC;;;SU)";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(kSddl, SDDL_REVISION_1, &descriptor, nullptr)) return false;
    const bool ok = SetServiceObjectSecurity(service, DACL_SECURITY_INFORMATION, descriptor) != FALSE;
    LocalFree(descriptor);
    return ok;
}

bool StopServiceHandle(SC_HANDLE service, DWORD timeoutMs) {
    SERVICE_STATUS status{};
    if (!ControlService(service, SERVICE_CONTROL_STOP, &status)) {
        const DWORD error = GetLastError();
        if (error != ERROR_SERVICE_NOT_ACTIVE && error != ERROR_SERVICE_CANNOT_ACCEPT_CTRL) return false;
    }
    for (DWORD waited = 0; waited < timeoutMs; waited += 250) {
        if (!QueryServiceStatus(service, &status)) return false;
        if (status.dwCurrentState == SERVICE_STOPPED) return true;
        Sleep(250);
    }
    return false;
}

bool InstallService() {
    wchar_t path[MAX_PATH]{}; GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring binaryPath = L"\"" + std::wstring(path) + L"\" --service";
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE | SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE service = CreateServiceW(scm, L"EnergyStarService", L"EnergyStar Core", SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, binaryPath.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
    if (!service && GetLastError() == ERROR_SERVICE_EXISTS) service = OpenServiceW(scm, L"EnergyStarService", SERVICE_ALL_ACCESS);
    bool ok = service != nullptr;
    if (ok) {
        ok = ChangeServiceConfigW(service, SERVICE_NO_CHANGE, SERVICE_AUTO_START, SERVICE_NO_CHANGE,
            binaryPath.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr) != FALSE;
        ok = ClearFailureActions(service) && ok;
        ok = SetServicePermissions(service) && ok;
        SERVICE_DESCRIPTIONW description{ const_cast<LPWSTR>(L"Keeps background applications efficient without slowing the active application.") };
        ChangeServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION, &description);
        SERVICE_STATUS_PROCESS status{};
        DWORD bytes = 0;
        const bool queried = QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
            reinterpret_cast<LPBYTE>(&status), sizeof(status), &bytes) != FALSE;
        const bool oldUncontrollableService = queried && status.dwCurrentState != SERVICE_STOPPED &&
            !(status.dwControlsAccepted & SERVICE_ACCEPT_STOP);
        if (ok && !oldUncontrollableService) ok = StopServiceHandle(service, 30000);
        if (ok && !oldUncontrollableService)
            ok = StartServiceW(service, 0, nullptr) != FALSE || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING;
        CloseServiceHandle(service);
    }
    CloseServiceHandle(scm);
    return ok;
}

bool RemoveService() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!scm) return false;
    SC_HANDLE service = OpenServiceW(scm, L"EnergyStarService",
        SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS | SERVICE_CHANGE_CONFIG);
    if (!service) {
        const bool missing = GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST;
        CloseServiceHandle(scm);
        return missing;
    }
    ClearFailureActions(service);
    if (!StopServiceHandle(service, 30000)) {
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        return false;
    }
    bool deleted = DeleteService(service) != FALSE;
    if (!deleted && GetLastError() == ERROR_SERVICE_MARKED_FOR_DELETE) deleted = true;
    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    return deleted;
}
}

int RunServiceProcess(const wchar_t* commandLine) {
    if (commandLine && _wcsicmp(commandLine, L"--install") == 0) return InstallService() ? 0 : 1;
    if (commandLine && _wcsicmp(commandLine, L"--uninstall") == 0) return RemoveService() ? 0 : 1;
    SERVICE_TABLE_ENTRYW table[] = { { const_cast<LPWSTR>(L"EnergyStarService"), ServiceMain }, { nullptr, nullptr } };
    return StartServiceCtrlDispatcherW(table) ? 0 : 1;
}
