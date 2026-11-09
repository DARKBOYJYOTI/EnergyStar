#pragma once
#include <windows.h>

#include "Theme.h"

enum NavIcon { NavPower = 0, NavWhitelist = 1, NavSettings = 2, NavAbout = 3 };

void EnableHover(HWND control);
void FillRounded(HDC dc, const RECT& rect, int radius, COLORREF fill, COLORREF border);
void DrawPrimaryButton(const Theme& theme, const DRAWITEMSTRUCT* draw);
void DrawSecondaryButton(const Theme& theme, const DRAWITEMSTRUCT* draw);
void DrawModeCard(const Theme& theme, const DRAWITEMSTRUCT* draw, const wchar_t* title, const wchar_t* detail);
void DrawToggleCard(const Theme& theme, const DRAWITEMSTRUCT* draw, const wchar_t* title, const wchar_t* detail);
void DrawNavItem(const Theme& theme, const DRAWITEMSTRUCT* draw, int icon, bool active);
void DrawListRow(const Theme& theme, const DRAWITEMSTRUCT* draw);
void DrawNavIcon(HDC dc, int icon, int left, int top, int size, COLORREF color);
void DrawStatusDot(HDC dc, int left, int top, int size, COLORREF color);
