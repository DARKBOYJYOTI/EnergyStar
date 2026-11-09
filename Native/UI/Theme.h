#pragma once
#include <windows.h>

struct Theme {
    bool dark = false;
    int dpi = 96;
    COLORREF content{}, sidebar{}, card{}, cardHover{}, cardSelected{}, border{}, borderSelected{};
    COLORREF accent{}, accentHover{}, accentPressed{};
    COLORREF text{}, textSecondary{}, textTertiary{};
    COLORREF navHover{}, navSelected{};
    COLORREF statusActive{}, statusInactive{};
    COLORREF divider{}, button{}, buttonHover{}, buttonPressed{};
    HFONT title{}, heading{}, semibold{}, body{}, caption{};
};

Theme CreateTheme(HWND window);
void DestroyTheme(Theme& theme);
int Dpi(const Theme& theme, int value);
void PaintText(HDC dc, HFONT font, COLORREF color, const RECT& area, const wchar_t* text, UINT format);
