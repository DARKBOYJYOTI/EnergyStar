#include "Theme.h"

#include <algorithm>

namespace {
HFONT MakeFont(const wchar_t* family, int points, int dpi, int weight) {
    return CreateFontW(-MulDiv(points, dpi, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, family);
}

int CALLBACK FontSeen(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM parameter) {
    *reinterpret_cast<bool*>(parameter) = true;
    return 0;
}

bool FontExists(const wchar_t* family) {
    HDC dc = GetDC(nullptr);
    LOGFONTW request{};
    request.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(request.lfFaceName, family);
    bool exists = false;
    EnumFontFamiliesExW(dc, &request, FontSeen, reinterpret_cast<LPARAM>(&exists), 0);
    ReleaseDC(nullptr, dc);
    return exists;
}

bool SystemUsesDarkTheme() {
    DWORD light = 1;
    DWORD size = sizeof(light);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size) != ERROR_SUCCESS)
        return false;
    return light == 0;
}
}

Theme CreateTheme(HWND window) {
    Theme theme;
    theme.dpi = window ? GetDpiForWindow(window) : 96;
    if (!theme.dpi) theme.dpi = 96;
    theme.dark = SystemUsesDarkTheme();

    if (theme.dark) {
        theme.content = RGB(30, 32, 36);
        theme.sidebar = RGB(23, 25, 29);
        theme.card = RGB(38, 41, 46);
        theme.cardHover = RGB(43, 46, 53);
        theme.cardSelected = RGB(43, 51, 66);
        theme.border = RGB(57, 61, 69);
        theme.borderSelected = RGB(96, 165, 250);
        theme.accent = RGB(59, 130, 246);
        theme.accentHover = RGB(96, 165, 250);
        theme.accentPressed = RGB(37, 99, 235);
        theme.text = RGB(237, 241, 247);
        theme.textSecondary = RGB(160, 170, 184);
        theme.textTertiary = RGB(124, 133, 146);
        theme.navHover = RGB(39, 43, 50);
        theme.navSelected = RGB(47, 56, 72);
        theme.statusActive = RGB(52, 199, 110);
        theme.statusInactive = RGB(124, 133, 146);
        theme.divider = RGB(49, 53, 60);
        theme.button = RGB(48, 51, 59);
        theme.buttonHover = RGB(57, 61, 70);
        theme.buttonPressed = RGB(66, 70, 80);
    } else {
        theme.content = RGB(248, 250, 253);
        theme.sidebar = RGB(241, 245, 251);
        theme.card = RGB(255, 255, 255);
        theme.cardHover = RGB(252, 253, 255);
        theme.cardSelected = RGB(237, 245, 255);
        theme.border = RGB(224, 230, 238);
        theme.borderSelected = RGB(59, 130, 246);
        theme.accent = RGB(37, 99, 235);
        theme.accentHover = RGB(29, 78, 216);
        theme.accentPressed = RGB(30, 64, 175);
        theme.text = RGB(23, 31, 42);
        theme.textSecondary = RGB(92, 104, 120);
        theme.textTertiary = RGB(138, 149, 164);
        theme.navHover = RGB(232, 239, 248);
        theme.navSelected = RGB(224, 235, 250);
        theme.statusActive = RGB(22, 163, 74);
        theme.statusInactive = RGB(148, 158, 170);
        theme.divider = RGB(229, 234, 242);
        theme.button = RGB(240, 243, 248);
        theme.buttonHover = RGB(231, 236, 244);
        theme.buttonPressed = RGB(222, 228, 238);
    }

    const int dpi = theme.dpi;
    const wchar_t* family = FontExists(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
    theme.title = MakeFont(family, 21, dpi, FW_SEMIBOLD);
    theme.heading = MakeFont(family, 13, dpi, FW_SEMIBOLD);
    theme.semibold = MakeFont(family, 10, dpi, FW_SEMIBOLD);
    theme.body = MakeFont(family, 10, dpi, FW_NORMAL);
    theme.small = MakeFont(family, 9, dpi, FW_NORMAL);
    return theme;
}

void DestroyTheme(Theme& theme) {
    for (HFONT* font : { &theme.title, &theme.heading, &theme.semibold, &theme.body, &theme.small }) {
        if (*font) { DeleteObject(*font); *font = nullptr; }
    }
}

int Dpi(const Theme& theme, int value) {
    if (theme.dpi <= 0) return value;
    return MulDiv(value, theme.dpi, 96);
}

void PaintText(HDC dc, HFONT font, COLORREF color, const RECT& area, const wchar_t* text, UINT format) {
    HGDIOBJ previous = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    RECT target = area;
    DrawTextW(dc, text, -1, &target, format);
    SelectObject(dc, previous);
}
