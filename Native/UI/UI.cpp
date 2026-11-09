#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <prsht.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <windowsx.h>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

#include "Resource.h"
#include "../Shared/Protocol.h"
#include "../App.h"

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Ole32.lib")

namespace {
constexpr UINT_PTR kStatusTimer = 1;

HINSTANCE g_instance{};
HBRUSH g_whiteBrush{};
bool g_serviceOk{};
bool g_active{};
bool g_logging{};
PowerMode g_mode = PowerMode::Automatic;
std::vector<std::wstring> g_exclusions;
std::wstring g_status = L"Status: Connecting to service…";
ULONGLONG g_savedUntil{};
HWND g_whitelistTip{};

void CopyTextToClipboard(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) return;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (data) {
        void* target = GlobalLock(data);
        if (target) {
            memcpy(target, text.c_str(), bytes);
            GlobalUnlock(data);
            if (SetClipboardData(CF_UNICODETEXT, data)) data = nullptr;
        }
    }
    if (data) GlobalFree(data);
    CloseClipboard();
}

bool RunElevatedCommand(HWND owner, const wchar_t* argument) {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    SHELLEXECUTEINFOW sei{ sizeof(sei) };
    sei.lpVerb = L"runas";
    sei.lpFile = path;
    sei.lpParameters = argument;
    sei.hwnd = owner;
    sei.nShow = SW_HIDE;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    if (!ShellExecuteExW(&sei)) return false;
    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 15000);
        DWORD exitCode = 1;
        GetExitCodeProcess(sei.hProcess, &exitCode);
        CloseHandle(sei.hProcess);
        return exitCode == 0;
    }
    return true;
}

LRESULT CALLBACK WhitelistListProc(HWND list, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR) {
    if (message == WM_CONTEXTMENU) {
        POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        int index = -1;
        if (point.x == -1 && point.y == -1) {
            index = static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
            if (index >= 0) {
                RECT itemRect{};
                if (SendMessageW(list, LB_GETITEMRECT, index, reinterpret_cast<LPARAM>(&itemRect)) != LB_ERR) {
                    POINT pt{ itemRect.left + 20, itemRect.top + (itemRect.bottom - itemRect.top) / 2 };
                    ClientToScreen(list, &pt);
                    point = pt;
                }
            }
        } else {
            POINT client = point;
            ScreenToClient(list, &client);
            const LRESULT hit = SendMessageW(list, LB_ITEMFROMPOINT, 0, MAKELPARAM(client.x, client.y));
            if (HIWORD(hit) == 0) index = LOWORD(hit);
        }
        if (index < 0 || index >= static_cast<int>(g_exclusions.size())) return 0;
        SendMessageW(list, LB_SETCURSEL, index, 0);

        HWND rootWindow = GetAncestor(list, GA_ROOT);
        RECT winRect{};
        if (!rootWindow || !GetWindowRect(rootWindow, &winRect)) {
            GetWindowRect(list, &winRect);
        }

        const bool alignRight = (point.x > (winRect.left + winRect.right) / 2);
        const bool alignBottom = (point.y > (winRect.top + winRect.bottom) / 2);

        LONG menuX = point.x;
        LONG menuY = point.y;
        menuX = std::clamp(menuX, winRect.left + 10, winRect.right - 10);
        menuY = std::clamp(menuY, winRect.top + 30, winRect.bottom - 10);

        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, IDM_COPY_PATH, L"Copy path");
        SetMenuDefaultItem(menu, IDM_COPY_PATH, FALSE);

        UINT flags = TPM_RETURNCMD | TPM_RIGHTBUTTON;
        flags |= (alignRight ? TPM_RIGHTALIGN : TPM_LEFTALIGN);
        flags |= (alignBottom ? TPM_BOTTOMALIGN : TPM_TOPALIGN);

        const int command = TrackPopupMenu(menu, flags, menuX, menuY, 0, list, nullptr);
        DestroyMenu(menu);
        if (command == IDM_COPY_PATH) CopyTextToClipboard(GetParent(list), g_exclusions[index]);
        return 0;
    }
    if (message == WM_KEYDOWN) {
        if (wParam == 'C' && (GetKeyState(VK_CONTROL) < 0)) {
            const int index = static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
            if (index >= 0 && index < static_cast<int>(g_exclusions.size())) {
                CopyTextToClipboard(GetParent(list), g_exclusions[index]);
                return 0;
            }
        }
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(list, WhitelistListProc, 1);
    return DefSubclassProc(list, message, wParam, lParam);
}

void UpdateServiceUi(HWND page) {
    ServiceState state = QueryEnergyStarServiceState();
    HWND statusLabel = GetDlgItem(page, IDC_SERVICE_STATUS);
    HWND installBtn = GetDlgItem(page, IDC_SERVICE_INSTALL);
    HWND toggleBtn = GetDlgItem(page, IDC_SERVICE_TOGGLE);

    if (!statusLabel || !installBtn || !toggleBtn) return;

    switch (state) {
    case ServiceState::NotInstalled:
        SetWindowTextW(statusLabel, L"Service status: Not installed");
        SetWindowTextW(installBtn, L"Install Service");
        EnableWindow(installBtn, TRUE);
        SetWindowTextW(toggleBtn, L"Start Service");
        EnableWindow(toggleBtn, FALSE);
        break;
    case ServiceState::Stopped:
        SetWindowTextW(statusLabel, L"Service status: Installed (Stopped)");
        SetWindowTextW(installBtn, L"Remove Service");
        EnableWindow(installBtn, TRUE);
        SetWindowTextW(toggleBtn, L"Start Service");
        EnableWindow(toggleBtn, TRUE);
        break;
    case ServiceState::Running:
        SetWindowTextW(statusLabel, L"Service status: Installed (Running)");
        SetWindowTextW(installBtn, L"Remove Service");
        EnableWindow(installBtn, TRUE);
        SetWindowTextW(toggleBtn, L"Stop Service");
        EnableWindow(toggleBtn, TRUE);
        break;
    case ServiceState::Starting:
        SetWindowTextW(statusLabel, L"Service status: Starting...");
        EnableWindow(installBtn, FALSE);
        EnableWindow(toggleBtn, FALSE);
        break;
    case ServiceState::Stopping:
        SetWindowTextW(statusLabel, L"Service status: Stopping...");
        EnableWindow(installBtn, FALSE);
        EnableWindow(toggleBtn, FALSE);
        break;
    }
}

void SetupWhitelistTooltip(HWND page) {
    HWND list = GetDlgItem(page, IDC_LIST);
    if (!list) return;
    g_whitelistTip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        page, nullptr, g_instance, nullptr);
    if (!g_whitelistTip) return;
    SetWindowPos(g_whitelistTip, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    TOOLINFOW tool{ sizeof(tool) };
    tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    tool.hwnd = page;
    tool.uId = reinterpret_cast<UINT_PTR>(list);
    tool.lpszText = LPSTR_TEXTCALLBACKW;
    SendMessageW(g_whitelistTip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
    SendMessageW(g_whitelistTip, TTM_SETMAXTIPWIDTH, 0, 600);
    SetWindowSubclass(list, WhitelistListProc, 1, 0);
}

bool CallService(PipeMessage& message) {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 3; ++attempt) {
        pipe = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_PIPE_BUSY) return false;
        WaitNamedPipeW(kPipeName, 500);
    }
    if (pipe == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0, read = 0;
    DWORD requestSize = message.action == PipeAction::Configure ? sizeof(message) : kHeaderSize;
    bool ok = WriteFile(pipe, &message, requestSize, &written, nullptr) && written == requestSize &&
        ReadFile(pipe, &message, sizeof(message), &read, nullptr) && read == sizeof(message);
    CloseHandle(pipe);
    return ok && message.magic == kProtocolMagic && message.foregroundPid != 0;
}

void UpdateStatusLine() {
    if (GetTickCount64() < g_savedUntil) {
        g_status = L"Status: Settings saved.";
        return;
    }
    if (!g_serviceOk) {
        ServiceState state = QueryEnergyStarServiceState();
        if (state == ServiceState::NotInstalled) {
            g_status = L"Status: Service not installed - install it in Settings";
        } else if (state == ServiceState::Stopped) {
            g_status = L"Status: Service stopped - start it in Settings";
        } else {
            g_status = L"Status: Connecting to service…";
        }
        g_active = false;
        return;
    }
    SYSTEM_POWER_STATUS power{};
    GetSystemPowerStatus(&power);
    if (!g_active) {
        g_status = L"Status: Inactive - power control is off";
        return;
    }
    g_status = power.ACLineStatus == 0 ? L"Status: Active - running on battery" : L"Status: Active - connected to power";
}

void RefreshStatus() {
    PipeMessage message{};
    message.action = PipeAction::Status;
    g_serviceOk = CallService(message);
    if (g_serviceOk) g_active = message.enabled != 0;
    UpdateStatusLine();
}

void LoadSettings() {
    PipeMessage message{};
    message.action = PipeAction::Status;
    g_serviceOk = CallService(message);
    if (!g_serviceOk) {
        g_active = false;
        UpdateStatusLine();
        return;
    }
    g_mode = message.mode;
    g_logging = message.logging != 0;
    g_active = message.enabled != 0;
    g_exclusions.clear();
    for (DWORD i = 0; i < message.exclusionCount && i < kMaxExclusions; ++i)
        g_exclusions.emplace_back(message.exclusions[i]);
    UpdateStatusLine();
}

bool SaveSettings(HWND owner) {
    PipeMessage message{};
    message.action = PipeAction::Configure;
    message.mode = g_mode;
    message.logging = g_logging ? 1 : 0;
    message.exclusionCount = static_cast<DWORD>(std::min<size_t>(g_exclusions.size(), kMaxExclusions));
    for (DWORD i = 0; i < message.exclusionCount; ++i) wcscpy_s(message.exclusions[i], g_exclusions[i].c_str());
    if (!CallService(message)) {
        MessageBoxW(owner, L"Could not update the EnergyStar service. Make sure setup completed.",
            L"EnergyStar", MB_ICONERROR | MB_OK);
        g_serviceOk = false;
        UpdateStatusLine();
        return false;
    }
    g_serviceOk = true;
    g_active = message.enabled != 0;
    g_savedUntil = GetTickCount64() + 2000;
    UpdateStatusLine();
    return true;
}

int ModeControlId(PowerMode mode) {
    if (mode == PowerMode::AlwaysOn) return IDC_MODE_ALWAYS;
    if (mode == PowerMode::Off) return IDC_MODE_OFF;
    return IDC_MODE_AUTO;
}

void DisplayStatus(HWND page) {
    SetDlgItemTextW(page, IDC_STATUS, g_status.c_str());
    ServiceState state = QueryEnergyStarServiceState();
    const bool serviceRunning = (state == ServiceState::Running);
    HWND autoRadio = GetDlgItem(page, IDC_MODE_AUTO);
    HWND alwaysRadio = GetDlgItem(page, IDC_MODE_ALWAYS);
    HWND offRadio = GetDlgItem(page, IDC_MODE_OFF);
    HWND actionBtn = GetDlgItem(page, IDC_POWER_ACTION);

    if (autoRadio) EnableWindow(autoRadio, serviceRunning);
    if (alwaysRadio) EnableWindow(alwaysRadio, serviceRunning);
    if (offRadio) EnableWindow(offRadio, serviceRunning);

    if (actionBtn) {
        if (state == ServiceState::NotInstalled) {
            SetWindowTextW(actionBtn, L"Install Service");
            ShowWindow(actionBtn, SW_SHOW);
            EnableWindow(actionBtn, TRUE);
        } else if (state == ServiceState::Stopped) {
            SetWindowTextW(actionBtn, L"Start Service");
            ShowWindow(actionBtn, SW_SHOW);
            EnableWindow(actionBtn, TRUE);
        } else if (state == ServiceState::Running) {
            ShowWindow(actionBtn, SW_HIDE);
        } else {
            SetWindowTextW(actionBtn, L"Please wait...");
            ShowWindow(actionBtn, SW_SHOW);
            EnableWindow(actionBtn, FALSE);
        }
    }
}

void RefreshList(HWND page) {
    HWND list = GetDlgItem(page, IDC_LIST);
    if (!list) return;
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    HDC dc = GetDC(list);
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(list, WM_GETFONT, 0, 0));
    HFONT oldFont = font ? reinterpret_cast<HFONT>(SelectObject(dc, font)) : nullptr;
    LONG maxExtent = 0;
    for (const auto& path : g_exclusions) {
        SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(path.c_str()));
        SIZE sz{};
        if (GetTextExtentPoint32W(dc, path.c_str(), static_cast<int>(path.length()), &sz)) {
            if (sz.cx > maxExtent) maxExtent = sz.cx;
        }
    }
    if (oldFont) SelectObject(dc, oldFont);
    ReleaseDC(list, dc);
    SendMessageW(list, LB_SETHORIZONTALEXTENT, maxExtent + 24, 0);
    const bool empty = g_exclusions.empty();
    ShowWindow(list, empty ? SW_HIDE : SW_SHOW);
    ShowWindow(GetDlgItem(page, IDC_EMPTY), empty ? SW_SHOW : SW_HIDE);
}

const wchar_t kHelpText[] =
    L"EnergyStar Quick Start & User Guide\r\n\r\n"
    L"1. HOW ENERGYSTAR WORKS\r\n"
    L"EnergyStar keeps background applications efficient without slowing down your active work. It uses Windows EcoQoS (Efficiency Mode) and idle process priority on background apps while boosting the active foreground application and its worker threads.\r\n\r\n"
    L"2. POWER MODES (Power Tab)\r\n"
    L"• Auto - battery only:\r\n"
    L"  Recommended for laptops. Power throttling only activates when your device is running on battery. When connected to AC power, applications run unrestricted.\r\n"
    L"• Always on:\r\n"
    L"  Keeps background power throttling active continuously, on both battery and AC power. Ideal for maximum thermal efficiency and quiet fan speeds.\r\n"
    L"• Off:\r\n"
    L"  Completely disables power control. All processes run at standard Windows defaults.\r\n\r\n"
    L"3. WHITELIST (Whitelist Tab)\r\n"
    L"• Why whitelist an app?\r\n"
    L"  Certain background apps (like video renderers, compilers, audio workstations, or game launchers) need full CPU power even when you switch windows.\r\n"
    L"• Adding an app:\r\n"
    L"  Click \"Add...\". You can pick any executable (.exe) or any shortcut (.lnk) from the Start Menu or your filesystem. Shortcuts are automatically resolved to the target executable.\r\n"
    L"• Removing an app:\r\n"
    L"  Select the application from the list and click \"Remove\".\r\n"
    L"• Copying path:\r\n"
    L"  Right-click an item in the list and choose \"Copy path\", or select it and press Ctrl+C.\r\n"
    L"• Scrolling:\r\n"
    L"  When paths are long or the list fills up, vertical and horizontal scrollbars allow full viewing.\r\n\r\n"
    L"4. DEBUG LOGGING (Settings Tab)\r\n"
    L"• By default, logging is turned OFF for maximum performance.\r\n"
    L"• To troubleshoot, check \"Enable debug log\" and click Apply.\r\n"
    L"• Logs are saved to EnergyStar.log beside the executable (Portable) or in C:\\Program Files\\EnergyStar (Installed).\r\n"
    L"• Logs contain ISO timestamps with timezone, process names, PIDs, boost events, and throttle events.\r\n\r\n"
    L"5. WINDOWS SERVICE & AGENT\r\n"
    L"• EnergyStarService runs in Session 0 as a background service to enforce EcoQoS policies.\r\n"
    L"• The Agent runs in your interactive user session to notify the service when you switch foreground windows.\r\n"
    L"• You can Install, Start, Stop, or Remove the service at any time from the Power or Settings tab.";

const wchar_t kAboutText[] =
    L"EnergyStar 2.0.0\r\n"
    L"Native Windows Efficiency & Battery Optimizer\r\n\r\n"
    L"Made with ❤️ by darkboyjyoti\r\n\r\n"
    L"PROJECT REPOSITORY:\r\n"
    L"https://github.com/DARKBOYJYOTI/EnergyStar\r\n\r\n"
    L"CREDITS & ATTRIBUTION:\r\n"
    L"Based on the concept and CLI tool by imbushuo:\r\n"
    L"https://github.com/imbushuo/EnergyStar\r\n"
    L"EnergyStar Core Service & Native GUI developed by darkboyjyoti.\r\n\r\n"
    L"PRIVACY POLICY:\r\n"
    L"• EnergyStar does not collect, track, store, or transmit any personal data, analytics, or telemetry.\r\n"
    L"• No network connections are initiated or accepted from outside your computer.\r\n"
    L"• All configuration files and diagnostic logs remain 100% local on your device.\r\n\r\n"
    L"TERMS OF USE & LICENSE:\r\n"
    L"Released under the MIT License.\r\n"
    L"Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files, to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software.\r\n\r\n"
    L"NO WARRANTY DISCLAIMER:\r\n"
    L"THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.";

std::wstring ResolveShortcut(const std::wstring& shortcutPath) {
    if (shortcutPath.length() < 4) return shortcutPath;
    const wchar_t* ext = wcsrchr(shortcutPath.c_str(), L'.');
    if (!ext || _wcsicmp(ext, L".lnk") != 0) return shortcutPath;

    IShellLinkW* shellLink = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shellLink)))) {
        return shortcutPath;
    }

    IPersistFile* persistFile = nullptr;
    std::wstring resolved;
    if (SUCCEEDED(shellLink->QueryInterface(IID_PPV_ARGS(&persistFile)))) {
        if (SUCCEEDED(persistFile->Load(shortcutPath.c_str(), STGM_READ))) {
            wchar_t targetPath[MAX_PATH]{};
            WIN32_FIND_DATAW findData{};
            if (SUCCEEDED(shellLink->GetPath(targetPath, MAX_PATH, &findData, SLGP_UNCPRIORITY))) {
                if (targetPath[0] != L'\0') {
                    resolved = targetPath;
                }
            }
        }
        persistFile->Release();
    }
    shellLink->Release();
    return resolved.empty() ? shortcutPath : resolved;
}

bool BrowseApplication(HWND owner) {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return false;
    COMDLG_FILTERSPEC filters[] = {
        { L"Applications and Shortcuts (*.exe; *.lnk)", L"*.exe;*.lnk" },
        { L"Applications (*.exe)", L"*.exe" },
        { L"Shortcuts (*.lnk)", L"*.lnk" },
        { L"All Files (*.*)", L"*.*" }
    };
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
    dialog->SetFileTypes(ARRAYSIZE(filters), filters);
    dialog->SetTitle(L"Choose an application to exclude");

    IShellItem* initialFolder = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(L"shell:AppsFolder", nullptr, IID_PPV_ARGS(&initialFolder)))) {
        dialog->SetDefaultFolder(initialFolder);
        initialFolder->Release();
    } else {
        PWSTR commonPrograms = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_CommonPrograms, 0, nullptr, &commonPrograms))) {
            if (SUCCEEDED(SHCreateItemFromParsingName(commonPrograms, nullptr, IID_PPV_ARGS(&initialFolder)))) {
                dialog->SetDefaultFolder(initialFolder);
                initialFolder->Release();
            }
            CoTaskMemFree(commonPrograms);
        }
    }

    if (FAILED(dialog->Show(owner))) { dialog->Release(); return false; }
    IShellItem* item = nullptr;
    if (FAILED(dialog->GetResult(&item))) { dialog->Release(); return false; }
    PWSTR selected = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &selected))) { item->Release(); dialog->Release(); return false; }
    std::wstring path(selected);
    CoTaskMemFree(selected);
    item->Release();
    dialog->Release();

    path = ResolveShortcut(path);
    const wchar_t* ext = wcsrchr(path.c_str(), L'.');
    if (!ext || _wcsicmp(ext, L".exe") != 0) {
        MessageBoxW(owner, L"Please select an executable (.exe) file or a shortcut pointing to one.",
            L"EnergyStar", MB_ICONWARNING | MB_OK);
        return false;
    }

    if (g_exclusions.size() >= kMaxExclusions) {
        MessageBoxW(owner, L"The whitelist is full.", L"EnergyStar", MB_ICONINFORMATION | MB_OK);
        return false;
    }
    if (std::find(g_exclusions.begin(), g_exclusions.end(), path) != g_exclusions.end()) return false;
    g_exclusions.emplace_back(path);
    return true;
}

void RemoveSelected(HWND page) {
    HWND list = GetDlgItem(page, IDC_LIST);
    const int selected = static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
    if (selected < 0 || selected >= static_cast<int>(g_exclusions.size())) return;
    g_exclusions.erase(g_exclusions.begin() + selected);
    RefreshList(page);
}

void RemoveSheetHelp(HWND sheet) {
    if (!sheet) return;
    LONG_PTR style = GetWindowLongPtrW(sheet, GWL_EXSTYLE);
    if (style & WS_EX_CONTEXTHELP) {
        SetWindowLongPtrW(sheet, GWL_EXSTYLE, style & ~static_cast<LONG_PTR>(WS_EX_CONTEXTHELP));
        SetWindowPos(sheet, nullptr, 0, 0, 0, 0,
            SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    HMENU menu = GetSystemMenu(sheet, FALSE);
    if (menu) DeleteMenu(menu, SC_CONTEXTHELP, MF_BYCOMMAND);
}

INT_PTR CALLBACK PageProc(HWND page, UINT message, WPARAM wParam, LPARAM lParam, int pageId) {
    switch (message) {
    case WM_INITDIALOG: {
        RemoveSheetHelp(GetParent(page));
        if (pageId == IDD_POWER) {
            CheckRadioButton(page, IDC_MODE_AUTO, IDC_MODE_OFF, ModeControlId(g_mode));
            DisplayStatus(page);
            SetTimer(page, kStatusTimer, 3000, nullptr);
            static bool s_prompted = false;
            if (!s_prompted) {
                s_prompted = true;
                ServiceState state = QueryEnergyStarServiceState();
                if (state == ServiceState::NotInstalled) {
                    if (MessageBoxW(page,
                            L"The EnergyStar background service is not installed.\n\nWould you like to install and start the service now?",
                            L"EnergyStar", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                        if (RunElevatedCommand(page, L"--install")) {
                            MessageBoxW(page, L"EnergyStar service installed and started successfully.", L"EnergyStar", MB_OK | MB_ICONINFORMATION);
                            LoadSettings();
                            RefreshStatus();
                            DisplayStatus(page);
                        }
                    }
                } else if (state == ServiceState::Stopped) {
                    if (MessageBoxW(page,
                            L"The EnergyStar background service is currently stopped.\n\nWould you like to start the service now?",
                            L"EnergyStar", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                        if (!StartEnergyStarService()) {
                            RunElevatedCommand(page, L"--start");
                        }
                        LoadSettings();
                        RefreshStatus();
                        DisplayStatus(page);
                    }
                }
            }
        } else if (pageId == IDD_WHITELIST) {
            RefreshList(page);
            SetupWhitelistTooltip(page);
        } else if (pageId == IDD_SETTINGS) {
            CheckDlgButton(page, IDC_LOGGING, g_logging ? BST_CHECKED : BST_UNCHECKED);
            UpdateServiceUi(page);
            SetTimer(page, kStatusTimer, 3000, nullptr);
        } else if (pageId == IDD_HELP) {
            SetDlgItemTextW(page, IDC_HELP_TEXT, kHelpText);
        } else if (pageId == IDD_ABOUT) {
            SetDlgItemTextW(page, IDC_ABOUT_LINKS,
                L"<a href=\"https://github.com/DARKBOYJYOTI/EnergyStar\">DARKBOYJYOTI/EnergyStar</a> | <a href=\"https://github.com/imbushuo/EnergyStar\">Original CLI</a>");
            SetDlgItemTextW(page, IDC_ABOUT_TEXT, kAboutText);
        }
        return TRUE;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        switch (id) {
        case IDC_MODE_AUTO:
            g_mode = PowerMode::Automatic;
            CheckRadioButton(page, IDC_MODE_AUTO, IDC_MODE_OFF, IDC_MODE_AUTO);
            PropSheet_Changed(GetParent(page), page);
            return TRUE;
        case IDC_MODE_ALWAYS:
            g_mode = PowerMode::AlwaysOn;
            CheckRadioButton(page, IDC_MODE_AUTO, IDC_MODE_OFF, IDC_MODE_ALWAYS);
            PropSheet_Changed(GetParent(page), page);
            return TRUE;
        case IDC_MODE_OFF:
            g_mode = PowerMode::Off;
            CheckRadioButton(page, IDC_MODE_AUTO, IDC_MODE_OFF, IDC_MODE_OFF);
            PropSheet_Changed(GetParent(page), page);
            return TRUE;
        case IDC_LOGGING:
            g_logging = IsDlgButtonChecked(page, IDC_LOGGING) == BST_CHECKED;
            PropSheet_Changed(GetParent(page), page);
            return TRUE;
        case IDC_ADD:
            if (BrowseApplication(page)) {
                RefreshList(page);
                PropSheet_Changed(GetParent(page), page);
            }
            return TRUE;
        case IDC_REMOVE:
            RemoveSelected(page);
            PropSheet_Changed(GetParent(page), page);
            return TRUE;
        case IDC_POWER_ACTION: {
            ServiceState state = QueryEnergyStarServiceState();
            if (state == ServiceState::NotInstalled) {
                if (RunElevatedCommand(page, L"--install")) {
                    MessageBoxW(page, L"EnergyStar service installed and started successfully.", L"EnergyStar", MB_OK | MB_ICONINFORMATION);
                    LoadSettings();
                } else {
                    MessageBoxW(page, L"Failed to install EnergyStar service. Administrator rights are required.", L"EnergyStar", MB_OK | MB_ICONWARNING);
                }
            } else if (state == ServiceState::Stopped) {
                if (!StartEnergyStarService()) {
                    RunElevatedCommand(page, L"--start");
                }
                LoadSettings();
            }
            Sleep(250);
            RefreshStatus();
            DisplayStatus(page);
            return TRUE;
        }
        case IDC_SERVICE_INSTALL: {
            ServiceState state = QueryEnergyStarServiceState();
            if (state == ServiceState::NotInstalled) {
                if (RunElevatedCommand(page, L"--install")) {
                    MessageBoxW(page, L"EnergyStar service installed and started successfully.", L"EnergyStar", MB_OK | MB_ICONINFORMATION);
                } else {
                    MessageBoxW(page, L"Failed to install EnergyStar service. Administrator rights are required.", L"EnergyStar", MB_OK | MB_ICONWARNING);
                }
            } else {
                if (MessageBoxW(page, L"Are you sure you want to remove the EnergyStar service?", L"EnergyStar", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                    if (RunElevatedCommand(page, L"--uninstall")) {
                        MessageBoxW(page, L"EnergyStar service removed successfully.", L"EnergyStar", MB_OK | MB_ICONINFORMATION);
                    } else {
                        MessageBoxW(page, L"Failed to remove EnergyStar service. Administrator rights are required.", L"EnergyStar", MB_OK | MB_ICONWARNING);
                    }
                }
            }
            RefreshStatus();
            UpdateServiceUi(page);
            return TRUE;
        }
        case IDC_SERVICE_TOGGLE: {
            ServiceState state = QueryEnergyStarServiceState();
            if (state == ServiceState::Running) {
                if (!StopEnergyStarService()) {
                    RunElevatedCommand(page, L"--stop");
                }
            } else if (state == ServiceState::Stopped) {
                if (!StartEnergyStarService()) {
                    RunElevatedCommand(page, L"--start");
                }
            }
            Sleep(250);
            RefreshStatus();
            UpdateServiceUi(page);
            return TRUE;
        }
        }
        return FALSE;
    }
    case WM_TIMER:
        if (wParam == kStatusTimer) {
            if (pageId == IDD_POWER) {
                RefreshStatus();
                DisplayStatus(page);
            } else if (pageId == IDD_SETTINGS) {
                UpdateServiceUi(page);
            }
            return TRUE;
        }
        return FALSE;
    case WM_CTLCOLORDLG:
        return reinterpret_cast<INT_PTR>(g_whiteBrush);
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetBkMode(dc, TRANSPARENT);
        SetBkColor(dc, RGB(255, 255, 255));
        return reinterpret_cast<INT_PTR>(g_whiteBrush);
    }
    case WM_NOTIFY: {
        auto* header = reinterpret_cast<NMHDR*>(lParam);
        if (!header) return FALSE;
        if (header->code == NM_CLICK || header->code == NM_RETURN) {
            if (header->idFrom == IDC_ABOUT_LINKS) {
                auto* link = reinterpret_cast<PNMLINK>(lParam);
                if (link && link->item.szUrl[0] != L'\0') {
                    ShellExecuteW(nullptr, L"open", link->item.szUrl, nullptr, nullptr, SW_SHOWNORMAL);
                    return TRUE;
                }
            }
        }
        if (pageId == IDD_WHITELIST && header->code == TTN_GETDISPINFOW && header->hwndFrom == g_whitelistTip) {
            auto* info = reinterpret_cast<NMTTDISPINFOW*>(lParam);
            HWND list = GetDlgItem(page, IDC_LIST);
            POINT point{};
            GetCursorPos(&point);
            ScreenToClient(list, &point);
            const LRESULT hit = SendMessageW(list, LB_ITEMFROMPOINT, 0, MAKELPARAM(point.x, point.y));
            const int index = HIWORD(hit) ? -1 : LOWORD(hit);
            if (index >= 0 && index < static_cast<int>(g_exclusions.size()))
                info->lpszText = const_cast<LPWSTR>(g_exclusions[index].c_str());
            else
                info->lpszText = const_cast<LPWSTR>(L"");
            SetWindowLongPtrW(page, DWLP_MSGRESULT, 0);
            return TRUE;
        }
        if (header->code == PSN_SETACTIVE) {
            if (pageId == IDD_POWER) {
                RefreshStatus();
                DisplayStatus(page);
            } else if (pageId == IDD_WHITELIST) {
                RefreshList(page);
            } else if (pageId == IDD_SETTINGS) {
                UpdateServiceUi(page);
            }
            SetWindowLongPtrW(page, DWLP_MSGRESULT, 0);
            return TRUE;
        }
        if (header->code == PSN_APPLY) {
            LRESULT result = PSNRET_NOERROR;
            if (pageId == IDD_POWER) {
                if (!SaveSettings(page)) result = PSNRET_INVALID;
                else DisplayStatus(page);
            }
            SetWindowLongPtrW(page, DWLP_MSGRESULT, result);
            return TRUE;
        }
        return FALSE;
    }
    case WM_DESTROY:
        KillTimer(page, kStatusTimer);
        if (pageId == IDD_WHITELIST && g_whitelistTip) {
            DestroyWindow(g_whitelistTip);
            g_whitelistTip = nullptr;
        }
        return TRUE;
    }
    return FALSE;
}

INT_PTR CALLBACK PowerPageProc(HWND page, UINT message, WPARAM wParam, LPARAM lParam) {
    return PageProc(page, message, wParam, lParam, IDD_POWER);
}

INT_PTR CALLBACK WhitelistPageProc(HWND page, UINT message, WPARAM wParam, LPARAM lParam) {
    return PageProc(page, message, wParam, lParam, IDD_WHITELIST);
}

INT_PTR CALLBACK SettingsPageProc(HWND page, UINT message, WPARAM wParam, LPARAM lParam) {
    return PageProc(page, message, wParam, lParam, IDD_SETTINGS);
}

INT_PTR CALLBACK HelpPageProc(HWND page, UINT message, WPARAM wParam, LPARAM lParam) {
    return PageProc(page, message, wParam, lParam, IDD_HELP);
}

INT_PTR CALLBACK AboutPageProc(HWND page, UINT message, WPARAM wParam, LPARAM lParam) {
    return PageProc(page, message, wParam, lParam, IDD_ABOUT);
}

int CALLBACK SheetCallback(HWND sheet, UINT message, LPARAM) {
    if (message == PSCB_INITIALIZED) {
        LONG_PTR style = GetWindowLongPtrW(sheet, GWL_EXSTYLE);
        SetWindowLongPtrW(sheet, GWL_EXSTYLE, style & ~static_cast<LONG_PTR>(WS_EX_CONTEXTHELP));
        HMENU menu = GetSystemMenu(sheet, FALSE);
        if (menu) DeleteMenu(menu, SC_CONTEXTHELP, MF_BYCOMMAND);
        SetWindowPos(sheet, nullptr, 0, 0, 0, 0,
            SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    return 0;
}
}

int RunUiProcess(HINSTANCE instance) {
    g_instance = instance;

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\EnergyStar.UI");
    if (!mutex) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        for (int attempt = 0; attempt < 20; ++attempt) {
            HWND existing = FindWindowW(L"#32770", L"EnergyStar");
            if (existing) {
                ShowWindow(existing, SW_RESTORE);
                SetForegroundWindow(existing);
                break;
            }
            Sleep(50);
        }
        CloseHandle(mutex);
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls{ sizeof(controls), ICC_WIN95_CLASSES | ICC_LINK_CLASS };
    InitCommonControlsEx(&controls);

    g_whiteBrush = CreateSolidBrush(RGB(255, 255, 255));
    LoadSettings();

    PROPSHEETPAGEW pages[5]{};
    const int templates[5] = { IDD_POWER, IDD_WHITELIST, IDD_SETTINGS, IDD_HELP, IDD_ABOUT };
    const DLGPROC procs[5] = { PowerPageProc, WhitelistPageProc, SettingsPageProc, HelpPageProc, AboutPageProc };
    for (int i = 0; i < 5; ++i) {
        pages[i].dwSize = sizeof(PROPSHEETPAGEW);
        pages[i].hInstance = instance;
        pages[i].pszTemplate = MAKEINTRESOURCEW(templates[i]);
        pages[i].pfnDlgProc = procs[i];
        pages[i].lParam = templates[i];
    }

    HICON icon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    PROPSHEETHEADERW sheet{};
    sheet.dwSize = sizeof(sheet);
    sheet.dwFlags = PSH_PROPSHEETPAGE | PSH_USEHICON | PSH_USECALLBACK;
    sheet.hInstance = instance;
    sheet.hIcon = icon;
    sheet.pszCaption = L"EnergyStar";
    sheet.nPages = 5;
    sheet.nStartPage = 0;
    sheet.ppsp = pages;
    sheet.pfnCallback = SheetCallback;

    const INT_PTR result = PropertySheetW(&sheet);

    if (icon) DestroyIcon(icon);
    if (g_whiteBrush) DeleteObject(g_whiteBrush);
    if (mutex) CloseHandle(mutex);
    CoUninitialize();
    return result < 0 ? 1 : 0;
}
