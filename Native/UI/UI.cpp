#include <windows.h>
#include <commctrl.h>
#include <prsht.h>
#include <shobjidl.h>
#include <windowsx.h>
#include <cstring>
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

LRESULT CALLBACK WhitelistListProc(HWND list, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR) {
    if (message == WM_CONTEXTMENU) {
        POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        int index = -1;
        if (point.x == -1 && point.y == -1) {
            index = static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
        } else {
            POINT client = point;
            ScreenToClient(list, &client);
            const LRESULT hit = SendMessageW(list, LB_ITEMFROMPOINT, 0, MAKELPARAM(client.x, client.y));
            if (HIWORD(hit) == 0) index = LOWORD(hit);
        }
        if (index < 0 || index >= static_cast<int>(g_exclusions.size())) return 0;
        SendMessageW(list, LB_SETCURSEL, index, 0);
        POINT screen{};
        GetCursorPos(&screen);
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, IDM_COPY_PATH, L"Copy path");
        SetMenuDefaultItem(menu, IDM_COPY_PATH, FALSE);
        SIZE textSize{};
        HDC dc = GetDC(list);
        if (dc) {
            GetTextExtentPoint32W(dc, L"Copy path", 9, &textSize);
            ReleaseDC(list, dc);
        }
        const int menuWidth = textSize.cx + GetSystemMetrics(SM_CXMENUCHECK) + 80;
        const int menuHeight = GetSystemMetrics(SM_CYMENU) + 8;
        HWND sheet = GetParent(GetParent(list));
        RECT client{};
        if (GetClientRect(sheet, &client)) {
            POINT corners[2] = { { client.left, client.top }, { client.right, client.bottom } };
            MapWindowPoints(sheet, nullptr, corners, 2);
            screen.x = std::clamp(screen.x, corners[0].x, std::max(corners[0].x, corners[1].x - menuWidth));
            screen.y = std::clamp(screen.y, corners[0].y, std::max(corners[0].y, corners[1].y - menuHeight));
        }
        const int command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, list, nullptr);
        DestroyMenu(menu);
        if (command == IDM_COPY_PATH) CopyTextToClipboard(GetParent(list), g_exclusions[index]);
        return 0;
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(list, WhitelistListProc, 1);
    return DefSubclassProc(list, message, wParam, lParam);
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
        g_status = L"Status: Service unavailable - start EnergyStar Core in Services";
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
        } else if (pageId == IDD_WHITELIST) {
            RefreshList(page);
            SetupWhitelistTooltip(page);
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
    sheet.dwFlags = PSH_PROPSHEETPAGE | PSH_USEHICON | PSH_USECALLBACK;
    sheet.hInstance = instance;
    sheet.hIcon = icon;
    sheet.pszCaption = L"EnergyStar";
    sheet.nPages = 4;
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
