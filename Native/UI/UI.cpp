#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <vector>
#include <string>
#include <algorithm>
#include "../Shared/Protocol.h"

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Comdlg32.lib")
#pragma comment(lib, "Shell32.lib")

namespace {
constexpr int kMode = 101, kLog = 102, kList = 103, kAdd = 104, kRemove = 105, kApply = 106, kStatus = 107;
HWND g_mode{}, g_log{}, g_list{}, g_status{};
HFONT g_font{};
std::vector<std::wstring> g_exclusions;

bool CallService(PipeMessage& message) {
    HANDLE pipe = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0, read = 0;
    DWORD requestSize = message.action == PipeAction::Configure ? sizeof(message) : kHeaderSize;
    bool ok = WriteFile(pipe, &message, requestSize, &written, nullptr) && written == requestSize &&
        ReadFile(pipe, &message, sizeof(message), &read, nullptr) && read == sizeof(message);
    CloseHandle(pipe);
    return ok && message.magic == kProtocolMagic;
}

void ApplyFont(HWND control) { SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE); }

void RefreshList() {
    SendMessageW(g_list, LB_RESETCONTENT, 0, 0);
    for (const auto& path : g_exclusions) SendMessageW(g_list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(path.c_str()));
}

void LoadSettings() {
    PipeMessage message{};
    message.action = PipeAction::Status;
    if (!CallService(message)) {
        SetWindowTextW(g_status, L"Core service is unavailable. Run setup as administrator.");
        return;
    }
    if (!message.foregroundPid) {
        MessageBoxW(nullptr, L"Service rejected configuration. Check whitelist paths and try again.", L"EnergyStar", MB_ICONERROR | MB_OK);
        return;
    }
    SendMessageW(g_mode, CB_SETCURSEL, static_cast<WPARAM>(message.mode), 0);
    SendMessageW(g_log, BM_SETCHECK, message.logging ? BST_CHECKED : BST_UNCHECKED, 0);
    g_exclusions.clear();
    for (DWORD i = 0; i < message.exclusionCount && i < kMaxExclusions; ++i) g_exclusions.emplace_back(message.exclusions[i]);
    RefreshList();
    SetWindowTextW(g_status, message.enabled ? (message.onBattery ? L"Active · running on battery" : L"Active · connected to power") : L"Paused · EcoQoS is off");
}

void SaveSettings() {
    PipeMessage message{};
    message.action = PipeAction::Configure;
    int selected = static_cast<int>(SendMessageW(g_mode, CB_GETCURSEL, 0, 0));
    message.mode = selected < 0 ? PowerMode::Automatic : static_cast<PowerMode>(selected);
    message.logging = SendMessageW(g_log, BM_GETCHECK, 0, 0) == BST_CHECKED;
    message.exclusionCount = static_cast<DWORD>(std::min<size_t>(g_exclusions.size(), kMaxExclusions));
    for (DWORD i = 0; i < message.exclusionCount; ++i) wcscpy_s(message.exclusions[i], g_exclusions[i].c_str());
    if (!CallService(message)) {
        MessageBoxW(nullptr, L"Could not update EnergyStar service.", L"EnergyStar", MB_ICONERROR | MB_OK);
        return;
    }
    if (!message.foregroundPid) {
        MessageBoxW(nullptr, L"Service rejected configuration. Check whitelist paths and try again.", L"EnergyStar", MB_ICONERROR | MB_OK);
        return;
    }
    SetWindowTextW(g_status, L"Settings saved and applied.");
}

void BrowseApplication() {
    wchar_t path[MAX_PATH]{};
    OPENFILENAMEW dialog{ sizeof(dialog) };
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrFilter = L"Applications (*.exe)\0*.exe\0All files\0*.*\0\0";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    dialog.lpstrTitle = L"Choose an application to exclude";
    if (!GetOpenFileNameW(&dialog)) return;
    if (g_exclusions.size() >= kMaxExclusions) { MessageBoxW(nullptr, L"Whitelist is full.", L"EnergyStar", MB_ICONINFORMATION); return; }
    if (std::find(g_exclusions.begin(), g_exclusions.end(), path) == g_exclusions.end()) g_exclusions.emplace_back(path);
    RefreshList();
}

HWND AddControl(HWND parent, const wchar_t* klass, const wchar_t* text, DWORD style, int id, int x, int y, int w, int h) {
    HWND control = CreateWindowExW(0, klass, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    ApplyFont(control);
    return control;
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_CREATE) {
        g_font = CreateFontW(-17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        AddControl(window, L"STATIC", L"EnergyStar", 0, 0, 28, 22, 250, 36);
        AddControl(window, L"STATIC", L"Power behavior", 0, 0, 28, 78, 150, 25);
        g_mode = AddControl(window, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, kMode, 28, 106, 340, 180);
        SendMessageW(g_mode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Auto — battery only"));
        SendMessageW(g_mode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Always on — force enable"));
        SendMessageW(g_mode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Off — disable throttling"));
        g_log = AddControl(window, L"BUTTON", L"Enable debug log", BS_AUTOCHECKBOX, kLog, 28, 152, 260, 28);
        AddControl(window, L"STATIC", L"Never throttle these applications", 0, 0, 28, 202, 360, 25);
        g_list = AddControl(window, L"LISTBOX", L"", LBS_NOTIFY | WS_BORDER | WS_VSCROLL, kList, 28, 232, 520, 230);
        AddControl(window, L"BUTTON", L"Browse…", BS_PUSHBUTTON, kAdd, 28, 478, 120, 34);
        AddControl(window, L"BUTTON", L"Remove", BS_PUSHBUTTON, kRemove, 160, 478, 120, 34);
        AddControl(window, L"BUTTON", L"Save settings", BS_DEFPUSHBUTTON, kApply, 390, 478, 158, 34);
        g_status = AddControl(window, L"STATIC", L"Connecting to service…", 0, kStatus, 28, 532, 520, 28);
        LoadSettings();
        return 0;
    }
    if (message == WM_COMMAND) {
        switch (LOWORD(wParam)) {
        case kAdd: BrowseApplication(); return 0;
        case kRemove: {
            int selected = static_cast<int>(SendMessageW(g_list, LB_GETCURSEL, 0, 0));
            if (selected >= 0 && selected < static_cast<int>(g_exclusions.size())) { g_exclusions.erase(g_exclusions.begin() + selected); RefreshList(); }
            return 0;
        }
        case kApply: SaveSettings(); return 0;
        }
    }
    if (message == WM_CLOSE) { DestroyWindow(window); return 0; }
    if (message == WM_DESTROY) { if (g_font) DeleteObject(g_font); PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, wParam, lParam);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    INITCOMMONCONTROLSEX controls{ sizeof(controls), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&controls);
    WNDCLASSW klass{};
    klass.lpfnWndProc = WindowProc;
    klass.hInstance = instance;
    klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    klass.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
    klass.hbrBackground = CreateSolidBrush(RGB(244, 246, 250));
    klass.lpszClassName = L"EnergyStarNativeWindow";
    RegisterClassW(&klass);
    HWND window = CreateWindowExW(0, klass.lpszClassName, L"EnergyStar", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 600, 650, nullptr, nullptr, instance, nullptr);
    if (!window) return 1;
    ShowWindow(window, show);
    UpdateWindow(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    DeleteObject(klass.hbrBackground);
    return static_cast<int>(message.wParam);
}
