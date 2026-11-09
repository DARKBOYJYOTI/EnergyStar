#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <string>
#include <vector>
#include <algorithm>
#include <iterator>

#include "Theme.h"
#include "UIControls.h"
#include "../Shared/Protocol.h"

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Dwmapi.lib")
#pragma comment(lib, "Advapi32.lib")

namespace {
constexpr int kPagePower = 0, kPageWhitelist = 1, kPageSettings = 2, kPageAbout = 3;

constexpr int kNavPower = 201, kNavWhitelist = 202, kNavSettings = 203, kNavAbout = 204;
constexpr int kModeAuto = 210, kModeAlways = 211, kModeOff = 212;
constexpr int kToggleLog = 213;
constexpr int kList = 214, kAdd = 215, kRemove = 216, kSave = 217;
constexpr int kEmpty = 218, kVersion = 219;

constexpr UINT_PTR kStatusTimer = 1, kToastTimer = 2;

HWND g_window{}, g_auto{}, g_always{}, g_off{}, g_toggle{}, g_list{}, g_empty{}, g_add{}, g_remove{}, g_save{};
HWND g_nav[4]{}, g_version{};
Theme g_theme{};
HBRUSH g_contentBrush{}, g_sidebarBrush{}, g_cardBrush{};
HICON g_appIcon{};
int g_page = kPagePower;
PowerMode g_mode = PowerMode::Automatic;
bool g_logging = false;
bool g_active = false;
bool g_serviceOk = false;
bool g_toast = false;
std::wstring g_status = L"Connecting to service…";
std::vector<std::wstring> g_exclusions;
RECT g_listCard{};

int S(int value) { return Dpi(g_theme, value); }

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

std::wstring DataDirectory() {
    wchar_t module[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, module, MAX_PATH)) {
        std::wstring folder(module);
        const size_t slash = folder.find_last_of(L"\\/");
        if (slash != std::wstring::npos) folder.resize(slash);
        if (GetFileAttributesW((folder + L"\\EnergyStar.installed").c_str()) == INVALID_FILE_ATTRIBUTES)
            return folder;
    }
    PWSTR known = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_CREATE, nullptr, &known))) {
        std::wstring folder = std::wstring(known) + L"\\EnergyStar";
        CoTaskMemFree(known);
        return folder;
    }
    return L".";
}

void UpdateStatusLine() {
    if (!g_serviceOk) { g_status = L"Service unavailable · run setup as administrator"; g_active = false; return; }
    SYSTEM_POWER_STATUS power{};
    GetSystemPowerStatus(&power);
    if (!g_active) { g_status = L"Inactive · power control is off"; return; }
    g_status = power.ACLineStatus == 0 ? L"Active · running on battery" : L"Active · connected to power";
}

void RefreshStatusOnly() {
    PipeMessage message{};
    message.action = PipeAction::Status;
    g_serviceOk = CallService(message);
    if (g_serviceOk) g_active = message.enabled != 0;
    UpdateStatusLine();
    InvalidateRect(g_window, nullptr, FALSE);
}

void ApplyModeChecks() {
    SendMessageW(g_auto, BM_SETCHECK, g_mode == PowerMode::Automatic ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_always, BM_SETCHECK, g_mode == PowerMode::AlwaysOn ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_off, BM_SETCHECK, g_mode == PowerMode::Off ? BST_CHECKED : BST_UNCHECKED, 0);
}

void RefreshList() {
    SendMessageW(g_list, LB_RESETCONTENT, 0, 0);
    for (const auto& path : g_exclusions)
        SendMessageW(g_list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(path.c_str()));
    const bool visible = g_page == kPageWhitelist && !g_exclusions.empty();
    ShowWindow(g_list, visible ? SW_SHOW : SW_HIDE);
    ShowWindow(g_empty, g_page == kPageWhitelist && g_exclusions.empty() ? SW_SHOW : SW_HIDE);
    InvalidateRect(g_window, &g_listCard, FALSE);
}

void LoadSettings() {
    PipeMessage message{};
    message.action = PipeAction::Status;
    g_serviceOk = CallService(message);
    if (!g_serviceOk) {
        g_active = false;
        g_status = L"Service unavailable · run setup as administrator";
        ApplyModeChecks();
        SendMessageW(g_toggle, BM_SETCHECK, BST_UNCHECKED, 0);
        return;
    }
    g_mode = message.mode;
    g_logging = message.logging != 0;
    g_active = message.enabled != 0;
    ApplyModeChecks();
    SendMessageW(g_toggle, BM_SETCHECK, g_logging ? BST_CHECKED : BST_UNCHECKED, 0);
    g_exclusions.clear();
    for (DWORD i = 0; i < message.exclusionCount && i < kMaxExclusions; ++i)
        g_exclusions.emplace_back(message.exclusions[i]);
    RefreshList();
    UpdateStatusLine();
    InvalidateRect(g_window, nullptr, FALSE);
}

bool SaveSettings() {
    PipeMessage message{};
    message.action = PipeAction::Configure;
    message.mode = g_mode;
    message.logging = g_logging ? 1 : 0;
    message.exclusionCount = static_cast<DWORD>(std::min<size_t>(g_exclusions.size(), kMaxExclusions));
    for (DWORD i = 0; i < message.exclusionCount; ++i) wcscpy_s(message.exclusions[i], g_exclusions[i].c_str());
    if (!CallService(message)) {
        MessageBoxW(g_window, L"Could not update the EnergyStar service. Make sure setup completed.",
            L"EnergyStar", MB_ICONERROR | MB_OK);
        g_serviceOk = false;
        UpdateStatusLine();
        InvalidateRect(g_window, nullptr, FALSE);
        return false;
    }
    g_serviceOk = true;
    g_active = message.enabled != 0;
    g_toast = true;
    SetTimer(g_window, kToastTimer, 1800, nullptr);
    UpdateStatusLine();
    InvalidateRect(g_window, nullptr, FALSE);
    return true;
}

void BrowseApplication() {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return;
    COMDLG_FILTERSPEC filters[] = { { L"Applications", L"*.exe" } };
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
    dialog->SetFileTypes(1, filters);
    dialog->SetTitle(L"Choose an application to exclude");
    if (FAILED(dialog->Show(g_window))) { dialog->Release(); return; }
    IShellItem* item = nullptr;
    if (FAILED(dialog->GetResult(&item))) { dialog->Release(); return; }
    PWSTR selected = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &selected))) { item->Release(); dialog->Release(); return; }
    std::wstring path(selected);
    CoTaskMemFree(selected);
    item->Release();
    dialog->Release();
    if (g_exclusions.size() >= kMaxExclusions) {
        MessageBoxW(g_window, L"The whitelist is full.", L"EnergyStar", MB_ICONINFORMATION | MB_OK);
        return;
    }
    if (std::find(g_exclusions.begin(), g_exclusions.end(), path) == g_exclusions.end()) {
        g_exclusions.emplace_back(path);
        RefreshList();
        SaveSettings();
    }
}

void RemoveSelected() {
    const int selected = static_cast<int>(SendMessageW(g_list, LB_GETCURSEL, 0, 0));
    if (selected < 0 || selected >= static_cast<int>(g_exclusions.size())) return;
    g_exclusions.erase(g_exclusions.begin() + selected);
    RefreshList();
    SaveSettings();
}

HWND AddControl(const wchar_t* klass, const wchar_t* text, DWORD style, int id, HFONT font) {
    HWND control = CreateWindowExW(0, klass, text, WS_CHILD | style, 0, 0, 0, 0, g_window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    if (control && font) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return control;
}

void ShowPageControls() {
    for (HWND control : { g_auto, g_always, g_off })
        ShowWindow(control, g_page == kPagePower ? SW_SHOW : SW_HIDE);
    ShowWindow(g_toggle, g_page == kPageSettings ? SW_SHOW : SW_HIDE);
    const bool whitelist = g_page == kPageWhitelist;
    ShowWindow(g_add, whitelist ? SW_SHOW : SW_HIDE);
    ShowWindow(g_remove, whitelist ? SW_SHOW : SW_HIDE);
    if (whitelist) {
        ShowWindow(g_list, g_exclusions.empty() ? SW_HIDE : SW_SHOW);
        ShowWindow(g_empty, g_exclusions.empty() ? SW_SHOW : SW_HIDE);
    } else {
        ShowWindow(g_list, SW_HIDE);
        ShowWindow(g_empty, SW_HIDE);
    }
    for (HWND nav : g_nav) InvalidateRect(nav, nullptr, FALSE);
}

void Layout() {
    if (!g_window) return;
    RECT client{};
    GetClientRect(g_window, &client);
    const int width = client.right, height = client.bottom;
    const int sidebar = S(224);
    const int pad = S(40);
    const int contentX = sidebar + pad;
    const int contentWidth = std::max(S(320), width - contentX - pad);
    const int footerHeight = S(88);
    const int footerTop = height - footerHeight;
    const int bodyTop = S(118);

    const int navX = S(14), navWidth = sidebar - S(28), navHeight = S(44), navGap = S(6), navTop = S(94);
    for (int i = 0; i < 4; ++i) {
        if (g_nav[i]) MoveWindow(g_nav[i], navX, navTop + i * (navHeight + navGap), navWidth, navHeight, TRUE);
    }
    if (g_version) MoveWindow(g_version, S(24), height - S(42), sidebar - S(48), S(22), TRUE);

    int cardHeight = S(88), cardGap = S(14);
    MoveWindow(g_auto, contentX, bodyTop, contentWidth, cardHeight, TRUE);
    MoveWindow(g_always, contentX, bodyTop + cardHeight + cardGap, contentWidth, cardHeight, TRUE);
    MoveWindow(g_off, contentX, bodyTop + 2 * (cardHeight + cardGap), contentWidth, cardHeight, TRUE);

    MoveWindow(g_toggle, contentX, bodyTop, contentWidth, S(84), TRUE);

    g_listCard = { contentX, bodyTop, contentX + contentWidth, footerTop - S(70) };
    RECT listRect = g_listCard;
    InflateRect(&listRect, -S(10), -S(10));
    MoveWindow(g_list, listRect.left, listRect.top, listRect.right - listRect.left, listRect.bottom - listRect.top, TRUE);
    MoveWindow(g_empty, listRect.left + S(16), listRect.top + (listRect.bottom - listRect.top) / 2 - S(12),
        listRect.right - listRect.left - S(32), S(24), TRUE);

    const int rowTop = g_listCard.bottom + S(14);
    MoveWindow(g_add, contentX, rowTop, S(168), S(40), TRUE);
    MoveWindow(g_remove, contentX + S(178), rowTop, S(120), S(40), TRUE);

    const int saveWidth = S(172), saveHeight = S(42);
    MoveWindow(g_save, width - pad - saveWidth, footerTop + (footerHeight - saveHeight) / 2, saveWidth, saveHeight, TRUE);
    ShowPageControls();
    InvalidateRect(g_window, nullptr, FALSE);
}

void PaintSidebar(HDC dc, const RECT& client) {
    const int sidebar = S(224);
    RECT bar{ 0, 0, sidebar, client.bottom };
    FillRect(dc, &bar, g_sidebarBrush);

    RECT divider{ sidebar - 1, 0, sidebar, client.bottom };
    HBRUSH dividerBrush = CreateSolidBrush(g_theme.divider);
    FillRect(dc, &divider, dividerBrush);
    DeleteObject(dividerBrush);

    const int iconSize = S(28);
    if (g_appIcon)
        DrawIconEx(dc, S(24), S(24), g_appIcon, iconSize, iconSize, 0, nullptr, DI_NORMAL);

    RECT name{ S(60), S(22), sidebar - S(12), S(22) + S(32) };
    PaintText(dc, g_theme.heading, g_theme.text, name, L"EnergyStar", DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

void PaintHeader(HDC dc, const RECT& client) {
    const int contentX = S(264);
    const wchar_t* title = L"Power";
    const wchar_t* subtitle = L"Choose when EnergyStar optimizes background applications.";
    if (g_page == kPageWhitelist) { title = L"Whitelist"; subtitle = L"Applications that must always keep full speed."; }
    else if (g_page == kPageSettings) { title = L"Settings"; subtitle = L"Logging and stored configuration."; }
    else if (g_page == kPageAbout) { title = L"About"; subtitle = L"Lightweight power control for Windows."; }

    RECT titleArea{ contentX, S(26), client.right - S(40), S(26) + S(38) };
    PaintText(dc, g_theme.title, g_theme.text, titleArea, title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT subtitleArea{ contentX, S(66), client.right - S(40), S(66) + S(24) };
    PaintText(dc, g_theme.caption, g_theme.textSecondary, subtitleArea, subtitle, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

void PaintPageBody(HDC dc, const RECT& client) {
    if (g_page == kPagePower) {
        RECT note{ S(264), S(118) + 3 * (S(88) + S(14)) + S(4), client.right - S(40), S(190) + 3 * (S(88) + S(14)) };
        PaintText(dc, g_theme.caption, g_theme.textTertiary, note,
            L"Changes apply immediately. Auto mode only throttles while running on battery.", DT_LEFT | DT_TOP | DT_WORDBREAK);
    } else if (g_page == kPageSettings) {
        const int left = S(264);
        const int top = S(118) + S(84) + S(26);
        const std::wstring directory = DataDirectory();
        RECT logLabel{ left, top, client.right - S(40), top + S(20) };
        PaintText(dc, g_theme.caption, g_theme.textTertiary, logLabel, L"LOG FILE", DT_LEFT | DT_TOP | DT_SINGLELINE);
        RECT logPath{ left, top + S(20), client.right - S(40), top + S(42) };
        PaintText(dc, g_theme.caption, g_theme.textSecondary, logPath, (directory + L"\\EnergyStar.log").c_str(),
            DT_LEFT | DT_TOP | DT_SINGLELINE | DT_PATH_ELLIPSIS);
        RECT configLabel{ left, top + S(56), client.right - S(40), top + S(76) };
        PaintText(dc, g_theme.caption, g_theme.textTertiary, configLabel, L"CONFIGURATION", DT_LEFT | DT_TOP | DT_SINGLELINE);
        RECT configPath{ left, top + S(76), client.right - S(40), top + S(98) };
        PaintText(dc, g_theme.caption, g_theme.textSecondary, configPath, (directory + L"\\EnergyStar.conf").c_str(),
            DT_LEFT | DT_TOP | DT_SINGLELINE | DT_PATH_ELLIPSIS);
    } else if (g_page == kPageAbout) {
        const std::wstring text =
            L"EnergyStar keeps background applications efficient without slowing the app you are using.\n\n"
            L"Version 2.0.0\n"
            L"Service: EnergyStar.Service.exe · Agent: EnergyStar.Agent.exe\n\n"
            L"Power control uses Windows EcoQoS and idle process priority. Windows may deny access to protected processes; "
            L"add those applications to the whitelist if they must stay responsive.";
        RECT area{ S(264), S(126), client.right - S(40), client.bottom - S(110) };
        PaintText(dc, g_theme.body, g_theme.textSecondary, area, text.c_str(), DT_LEFT | DT_TOP | DT_WORDBREAK);
    }
}

void PaintFooter(HDC dc, const RECT& client) {
    const int footerTop = client.bottom - S(88);
    RECT line{ S(224), footerTop, client.right, footerTop + 1 };
    HBRUSH brush = CreateSolidBrush(g_theme.divider);
    FillRect(dc, &line, brush);
    DeleteObject(brush);

    const int dotSize = S(9);
    const int dotLeft = S(264);
    const int middle = footerTop + S(44);
    DrawStatusDot(dc, dotLeft, middle - dotSize / 2, dotSize,
        g_active ? g_theme.statusActive : g_theme.statusInactive);
    RECT statusArea{ dotLeft + dotSize + S(10), middle - S(14), client.right - S(260), middle + S(14) };
    PaintText(dc, g_theme.body, g_theme.textSecondary, statusArea, g_status.c_str(),
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    if (g_toast) {
        RECT toast{ client.right - S(40) - S(172) - S(96), middle - S(14), client.right - S(40) - S(172) - S(8), middle + S(14) };
        PaintText(dc, g_theme.caption, g_theme.statusActive, toast, L"Saved", DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
}

void PaintWindow(HWND window) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);

    FillRect(dc, &client, g_contentBrush);
    PaintSidebar(dc, client);
    PaintHeader(dc, client);
    PaintPageBody(dc, client);
    PaintFooter(dc, client);

    if (g_page == kPageWhitelist) FillRounded(dc, g_listCard, S(10), g_theme.card, g_theme.border);

    EndPaint(window, &paint);
}

void Navigate(int page) {
    if (g_page == page) return;
    g_page = page;
    Layout();
}

void ApplyDwm(HWND window) {
    const DWORD corner = 2;
    DwmSetWindowAttribute(window, 33, &corner, sizeof(corner));
    const BOOL dark = g_theme.dark ? TRUE : FALSE;
    DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        g_window = window;
        g_theme = CreateTheme(window);
        g_contentBrush = CreateSolidBrush(g_theme.content);
        g_sidebarBrush = CreateSolidBrush(g_theme.sidebar);
        g_cardBrush = CreateSolidBrush(g_theme.card);
        g_appIcon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON,
            S(32), S(32), LR_DEFAULTSIZE));
        ApplyDwm(window);

        g_nav[0] = AddControl(L"BUTTON", L"Power", BS_OWNERDRAW | WS_TABSTOP, kNavPower, g_theme.semibold);
        g_nav[1] = AddControl(L"BUTTON", L"Whitelist", BS_OWNERDRAW | WS_TABSTOP, kNavWhitelist, g_theme.semibold);
        g_nav[2] = AddControl(L"BUTTON", L"Settings", BS_OWNERDRAW | WS_TABSTOP, kNavSettings, g_theme.semibold);
        g_nav[3] = AddControl(L"BUTTON", L"About", BS_OWNERDRAW | WS_TABSTOP, kNavAbout, g_theme.semibold);
        for (HWND nav : g_nav) EnableHover(nav);

        g_auto = AddControl(L"BUTTON", L"Auto · battery only", BS_AUTORADIOBUTTON | BS_OWNERDRAW | WS_GROUP | WS_TABSTOP, kModeAuto, g_theme.body);
        g_always = AddControl(L"BUTTON", L"Always on", BS_AUTORADIOBUTTON | BS_OWNERDRAW | WS_TABSTOP, kModeAlways, g_theme.body);
        g_off = AddControl(L"BUTTON", L"Off", BS_AUTORADIOBUTTON | BS_OWNERDRAW | WS_TABSTOP, kModeOff, g_theme.body);
        for (HWND card : { g_auto, g_always, g_off }) EnableHover(card);

        g_toggle = AddControl(L"BUTTON", L"Enable debug log", BS_AUTOCHECKBOX | BS_OWNERDRAW | WS_TABSTOP, kToggleLog, g_theme.body);
        EnableHover(g_toggle);

        g_list = AddControl(L"LISTBOX", L"", LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOTIFY | WS_VSCROLL | WS_TABSTOP, kList, g_theme.body);
        SendMessageW(g_list, LB_SETITEMHEIGHT, 0, S(34));
        g_empty = AddControl(L"STATIC", L"No applications yet · add one below", SS_CENTER, kEmpty, g_theme.caption);

        g_add = AddControl(L"BUTTON", L"Add application…", BS_OWNERDRAW | WS_TABSTOP, kAdd, g_theme.semibold);
        g_remove = AddControl(L"BUTTON", L"Remove", BS_OWNERDRAW | WS_TABSTOP, kRemove, g_theme.semibold);
        g_save = AddControl(L"BUTTON", L"Save changes", BS_OWNERDRAW | BS_DEFPUSHBUTTON | WS_TABSTOP, kSave, g_theme.semibold);
        for (HWND button : { g_add, g_remove, g_save }) EnableHover(button);

        g_version = AddControl(L"STATIC", L"Version 2.0.0", SS_LEFT, kVersion, g_theme.caption);

        LoadSettings();
        Layout();
        SetTimer(window, kStatusTimer, 5000, nullptr);
        return 0;
    }
    case WM_SIZE:
        Layout();
        return 0;
    case WM_DPICHANGED: {
        DestroyTheme(g_theme);
        g_theme = CreateTheme(window);
        for (HWND control : { g_auto, g_always, g_off, g_toggle, g_list, g_add, g_remove, g_save })
            if (control) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_theme.body), TRUE);
        if (g_version) SendMessageW(g_version, WM_SETFONT, reinterpret_cast<WPARAM>(g_theme.caption), TRUE);
        if (g_empty) SendMessageW(g_empty, WM_SETFONT, reinterpret_cast<WPARAM>(g_theme.caption), TRUE);
        for (HWND nav : g_nav)
            if (nav) SendMessageW(nav, WM_SETFONT, reinterpret_cast<WPARAM>(g_theme.semibold), TRUE);
        if (g_list) SendMessageW(g_list, LB_SETITEMHEIGHT, 0, S(34));
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(window, nullptr, suggested->left, suggested->top,
            suggested->right - suggested->left, suggested->bottom - suggested->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        ApplyDwm(window);
        Layout();
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = S(800);
        info->ptMinTrackSize.y = S(620);
        return 0;
    }
    case WM_PAINT:
        PaintWindow(window);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetBkMode(dc, TRANSPARENT);
        const int id = GetDlgCtrlID(reinterpret_cast<HWND>(lParam));
        if (id == kEmpty) {
            SetTextColor(dc, g_theme.textSecondary);
            return reinterpret_cast<LRESULT>(g_cardBrush);
        }
        SetTextColor(dc, id == kVersion ? g_theme.textTertiary : g_theme.textSecondary);
        return reinterpret_cast<LRESULT>(id == kVersion ? g_sidebarBrush : g_contentBrush);
    }
    case WM_CTLCOLORLISTBOX: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetBkColor(dc, g_theme.card);
        SetTextColor(dc, g_theme.text);
        return reinterpret_cast<LRESULT>(g_cardBrush);
    }
    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        if (id == kNavPower) { Navigate(kPagePower); return 0; }
        if (id == kNavWhitelist) { Navigate(kPageWhitelist); return 0; }
        if (id == kNavSettings) { Navigate(kPageSettings); return 0; }
        if (id == kNavAbout) { Navigate(kPageAbout); return 0; }
        if (id == kModeAuto || id == kModeAlways || id == kModeOff) {
            g_mode = id == kModeAuto ? PowerMode::Automatic : id == kModeAlways ? PowerMode::AlwaysOn : PowerMode::Off;
            ApplyModeChecks();
            SaveSettings();
            return 0;
        }
        if (id == kToggleLog) {
            g_logging = SendMessageW(g_toggle, BM_GETCHECK, 0, 0) == BST_CHECKED;
            SaveSettings();
            return 0;
        }
        if (id == kAdd) { BrowseApplication(); return 0; }
        if (id == kRemove) { RemoveSelected(); return 0; }
        if (id == kSave) { SaveSettings(); return 0; }
        if (id == kList && HIWORD(wParam) == LBN_SELCHANGE) { InvalidateRect(g_list, nullptr, FALSE); return 0; }
        return 0;
    }
    case WM_DRAWITEM: {
        auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        switch (draw->CtlID) {
        case kNavPower: DrawNavItem(g_theme, draw, NavPower, g_page == kPagePower); return TRUE;
        case kNavWhitelist: DrawNavItem(g_theme, draw, NavWhitelist, g_page == kPageWhitelist); return TRUE;
        case kNavSettings: DrawNavItem(g_theme, draw, NavSettings, g_page == kPageSettings); return TRUE;
        case kNavAbout: DrawNavItem(g_theme, draw, NavAbout, g_page == kPageAbout); return TRUE;
        case kModeAuto: DrawModeCard(g_theme, draw, L"Auto · battery only", L"Optimize background apps while on battery."); return TRUE;
        case kModeAlways: DrawModeCard(g_theme, draw, L"Always on", L"Keep power control active at all times."); return TRUE;
        case kModeOff: DrawModeCard(g_theme, draw, L"Off", L"Disable power control completely."); return TRUE;
        case kToggleLog: DrawToggleCard(g_theme, draw, L"Enable debug log", L"Save detailed logs for troubleshooting."); return TRUE;
        case kAdd: DrawSecondaryButton(g_theme, draw); return TRUE;
        case kRemove: DrawSecondaryButton(g_theme, draw); return TRUE;
        case kSave: DrawPrimaryButton(g_theme, draw); return TRUE;
        case kList: DrawListRow(g_theme, draw); return TRUE;
        }
        return FALSE;
    }
    case WM_TIMER:
        if (wParam == kToastTimer) { KillTimer(window, kToastTimer); g_toast = false; InvalidateRect(window, nullptr, FALSE); return 0; }
        if (wParam == kStatusTimer) { RefreshStatusOnly(); return 0; }
        return 0;
    case WM_DESTROY:
        KillTimer(window, kStatusTimer);
        KillTimer(window, kToastTimer);
        if (g_appIcon) DestroyIcon(g_appIcon);
        DestroyTheme(g_theme);
        if (g_contentBrush) DeleteObject(g_contentBrush);
        if (g_sidebarBrush) DeleteObject(g_sidebarBrush);
        if (g_cardBrush) DeleteObject(g_cardBrush);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls{ sizeof(controls), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&controls);

    WNDCLASSEXW klass{ sizeof(klass) };
    klass.lpfnWndProc = WindowProc;
    klass.hInstance = instance;
    klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    klass.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
    klass.hIconSm = klass.hIcon;
    klass.hbrBackground = nullptr;
    klass.lpszClassName = L"EnergyStarNativeWindow";
    RegisterClassExW(&klass);

    HWND window = CreateWindowExW(WS_EX_APPWINDOW, klass.lpszClassName, L"EnergyStar",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 1000, 700,
        nullptr, nullptr, instance, nullptr);
    if (!window) return 1;
    ShowWindow(window, show);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    CoUninitialize();
    return static_cast<int>(message.wParam);
}
