#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <vector>
#include <string>
#include <algorithm>
#include <iterator>
#include "../Shared/Protocol.h"

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Comdlg32.lib")
#pragma comment(lib, "Shell32.lib")

namespace {
constexpr int kAuto = 101, kAlways = 108, kOff = 109, kLog = 102, kList = 103, kAdd = 104, kRemove = 105, kApply = 106, kStatus = 107;
HWND g_auto{}, g_always{}, g_off{}, g_log{}, g_list{}, g_status{};
HFONT g_font{};
HBRUSH g_background{};
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
    SendMessageW(message.mode == PowerMode::Automatic ? g_auto : message.mode == PowerMode::AlwaysOn ? g_always : g_off,
        BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(g_log, BM_SETCHECK, message.logging ? BST_CHECKED : BST_UNCHECKED, 0);
    g_exclusions.clear();
    for (DWORD i = 0; i < message.exclusionCount && i < kMaxExclusions; ++i) g_exclusions.emplace_back(message.exclusions[i]);
    RefreshList();
    SetWindowTextW(g_status, message.enabled ? (message.onBattery ? L"Active · running on battery" : L"Active · connected to power") : L"Paused · EcoQoS is off");
}

void SaveSettings() {
    PipeMessage message{};
    message.action = PipeAction::Configure;
    message.mode = SendMessageW(g_always, BM_GETCHECK, 0, 0) == BST_CHECKED ? PowerMode::AlwaysOn :
        SendMessageW(g_off, BM_GETCHECK, 0, 0) == BST_CHECKED ? PowerMode::Off : PowerMode::Automatic;
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
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return;
    COMDLG_FILTERSPEC filters[] = { { L"Applications", L"*.exe" } };
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
    dialog->SetFileTypes(1, filters);
    dialog->SetTitle(L"Choose an application to exclude");
    if (FAILED(dialog->Show(GetAncestor(g_list, GA_ROOT)))) { dialog->Release(); return; }
    IShellItem* item = nullptr;
    if (FAILED(dialog->GetResult(&item))) { dialog->Release(); return; }
    PWSTR selectedPath = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &selectedPath))) { item->Release(); dialog->Release(); return; }
    std::wstring path(selectedPath);
    CoTaskMemFree(selectedPath);
    item->Release();
    dialog->Release();
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

void DrawRoundedButton(const DRAWITEMSTRUCT* draw) {
    const bool primary = draw->CtlID == kApply;
    const bool pressed = (draw->itemState & ODS_SELECTED) != 0;
    RECT rect = draw->rcItem;
    HBRUSH fill = CreateSolidBrush(primary ? (pressed ? RGB(24, 91, 170) : RGB(35, 112, 204)) : RGB(232, 237, 244));
    HPEN border = CreatePen(PS_SOLID, 1, primary ? RGB(35, 112, 204) : RGB(215, 222, 232));
    HGDIOBJ oldBrush = SelectObject(draw->hDC, fill);
    HGDIOBJ oldPen = SelectObject(draw->hDC, border);
    RoundRect(draw->hDC, rect.left, rect.top, rect.right, rect.bottom, 12, 12);
    SelectObject(draw->hDC, oldBrush);
    SelectObject(draw->hDC, oldPen);
    DeleteObject(fill);
    DeleteObject(border);
    SetBkMode(draw->hDC, TRANSPARENT);
    SetTextColor(draw->hDC, primary ? RGB(255, 255, 255) : RGB(31, 41, 55));
    HGDIOBJ oldFont = SelectObject(draw->hDC, g_font);
    wchar_t label[128]{};
    GetWindowTextW(draw->hwndItem, label, static_cast<int>(std::size(label)));
    DrawTextW(draw->hDC, label, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(draw->hDC, oldFont);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_CREATE) {
        g_background = CreateSolidBrush(RGB(246, 248, 251));
        g_font = CreateFontW(-17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        AddControl(window, L"STATIC", L"EnergyStar", 0, 0, 28, 22, 250, 36);
        AddControl(window, L"STATIC", L"POWER MODE", 0, 0, 28, 76, 200, 25);
        g_auto = AddControl(window, L"BUTTON", L"Auto · battery only", BS_AUTORADIOBUTTON | WS_GROUP, kAuto, 30, 108, 190, 30);
        g_always = AddControl(window, L"BUTTON", L"Always on", BS_AUTORADIOBUTTON, kAlways, 230, 108, 150, 30);
        g_off = AddControl(window, L"BUTTON", L"Off", BS_AUTORADIOBUTTON, kOff, 390, 108, 100, 30);
        g_log = AddControl(window, L"BUTTON", L"Enable debug log", BS_AUTOCHECKBOX, kLog, 28, 158, 260, 28);
        AddControl(window, L"STATIC", L"WHITELIST", 0, 0, 28, 204, 200, 25);
        g_list = AddControl(window, L"LISTBOX", L"", LBS_NOTIFY | WS_BORDER | WS_VSCROLL, kList, 28, 234, 520, 230);
        AddControl(window, L"BUTTON", L"Add application…", BS_OWNERDRAW, kAdd, 28, 478, 160, 38);
        AddControl(window, L"BUTTON", L"Remove", BS_OWNERDRAW, kRemove, 200, 478, 120, 38);
        AddControl(window, L"BUTTON", L"Save changes", BS_OWNERDRAW, kApply, 390, 478, 158, 38);
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
        case kAuto: case kAlways: case kOff: SaveSettings(); return 0;
        }
    }
    if (message == WM_DRAWITEM && wParam != kList) {
        DrawRoundedButton(reinterpret_cast<const DRAWITEMSTRUCT*>(lParam));
        return TRUE;
    }
    if (message == WM_CLOSE) { DestroyWindow(window); return 0; }
    if (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLORBTN) {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetBkColor(dc, RGB(246, 248, 251));
        SetTextColor(dc, RGB(31, 41, 55));
        return reinterpret_cast<LRESULT>(g_background);
    }
    if (message == WM_ERASEBKGND) {
        RECT area{}; GetClientRect(window, &area);
        FillRect(reinterpret_cast<HDC>(wParam), &area, g_background);
        return 1;
    }
    if (message == WM_DESTROY) { if (g_font) DeleteObject(g_font); if (g_background) DeleteObject(g_background); PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, wParam, lParam);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
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
    CoUninitialize();
    return static_cast<int>(message.wParam);
}
