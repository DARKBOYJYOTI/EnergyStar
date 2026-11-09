#include <windows.h>
#include <commctrl.h>
#include <prsht.h>
#include <shobjidl.h>
#include <string>
#include <vector>
#include <algorithm>

#include "Resource.h"
#include "../Shared/Protocol.h"

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

bool CallService(PipeMessage& message) {
    HANDLE pipe = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
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
        g_status = L"Status: Service unavailable - run setup as administrator";
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
}

void RefreshList(HWND page) {
    HWND list = GetDlgItem(page, IDC_LIST);
    if (!list) return;
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    for (const auto& path : g_exclusions)
        SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(path.c_str()));
    const bool empty = g_exclusions.empty();
    ShowWindow(list, empty ? SW_HIDE : SW_SHOW);
    ShowWindow(GetDlgItem(page, IDC_EMPTY), empty ? SW_SHOW : SW_HIDE);
}

bool BrowseApplication(HWND owner) {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return false;
    COMDLG_FILTERSPEC filters[] = { { L"Applications", L"*.exe" } };
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
    dialog->SetFileTypes(1, filters);
    dialog->SetTitle(L"Choose an application to exclude");
    if (FAILED(dialog->Show(owner))) { dialog->Release(); return false; }
    IShellItem* item = nullptr;
    if (FAILED(dialog->GetResult(&item))) { dialog->Release(); return false; }
    PWSTR selected = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &selected))) { item->Release(); dialog->Release(); return false; }
    std::wstring path(selected);
    CoTaskMemFree(selected);
    item->Release();
    dialog->Release();
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

INT_PTR CALLBACK PageProc(HWND page, UINT message, WPARAM wParam, LPARAM lParam, int pageId) {
    switch (message) {
    case WM_INITDIALOG: {
        if (pageId == IDD_POWER) {
            CheckRadioButton(page, IDC_MODE_AUTO, IDC_MODE_OFF, ModeControlId(g_mode));
            DisplayStatus(page);
            SetTimer(page, kStatusTimer, 3000, nullptr);
        } else if (pageId == IDD_WHITELIST) {
            RefreshList(page);
        } else if (pageId == IDD_SETTINGS) {
            CheckDlgButton(page, IDC_LOGGING, g_logging ? BST_CHECKED : BST_UNCHECKED);
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
        }
        return FALSE;
    }
    case WM_TIMER:
        if (wParam == kStatusTimer) {
            RefreshStatus();
            DisplayStatus(page);
            return TRUE;
        }
        return FALSE;
    case WM_CTLCOLORDLG:
        return reinterpret_cast<INT_PTR>(g_whiteBrush);
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetBkMode(dc, TRANSPARENT);
        SetBkColor(dc, RGB(255, 255, 255));
        return reinterpret_cast<INT_PTR>(g_whiteBrush);
    }
    case WM_NOTIFY: {
        auto* header = reinterpret_cast<NMHDR*>(lParam);
        if (!header) return FALSE;
        if (header->code == PSN_SETACTIVE) {
            if (pageId == IDD_POWER) {
                RefreshStatus();
                DisplayStatus(page);
            } else if (pageId == IDD_WHITELIST) {
                RefreshList(page);
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

INT_PTR CALLBACK AboutPageProc(HWND page, UINT message, WPARAM wParam, LPARAM lParam) {
    return PageProc(page, message, wParam, lParam, IDD_ABOUT);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    g_instance = instance;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls{ sizeof(controls), ICC_WIN95_CLASSES };
    InitCommonControlsEx(&controls);

    g_whiteBrush = CreateSolidBrush(RGB(255, 255, 255));
    LoadSettings();

    PROPSHEETPAGEW pages[4]{};
    const int templates[4] = { IDD_POWER, IDD_WHITELIST, IDD_SETTINGS, IDD_ABOUT };
    const DLGPROC procs[4] = { PowerPageProc, WhitelistPageProc, SettingsPageProc, AboutPageProc };
    for (int i = 0; i < 4; ++i) {
        pages[i].dwSize = sizeof(PROPSHEETPAGEW);
        pages[i].hInstance = instance;
        pages[i].pszTemplate = MAKEINTRESOURCEW(templates[i]);
        pages[i].pfnDlgProc = procs[i];
        pages[i].lParam = templates[i];
    }

    HICON icon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    PROPSHEETHEADERW sheet{};
    sheet.dwSize = sizeof(sheet);
    sheet.dwFlags = PSH_PROPSHEETPAGE | PSH_PROPTITLE | PSH_USEHICON;
    sheet.hInstance = instance;
    sheet.hIcon = icon;
    sheet.pszCaption = L"EnergyStar";
    sheet.nPages = 4;
    sheet.nStartPage = 0;
    sheet.ppsp = pages;

    const INT_PTR result = PropertySheetW(&sheet);

    if (icon) DestroyIcon(icon);
    if (g_whiteBrush) DeleteObject(g_whiteBrush);
    CoUninitialize();
    return result < 0 ? 1 : 0;
}
