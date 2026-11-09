#include "UIControls.h"

#include <commctrl.h>
#include <cmath>
#include <iterator>

namespace {
constexpr LONG_PTR kHover = 1;

LRESULT CALLBACK TrackHover(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR) {
    switch (message) {
    case WM_MOUSEMOVE:
        if (!GetWindowLongPtrW(window, GWLP_USERDATA)) {
            TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, window, 0 };
            TrackMouseEvent(&track);
            SetWindowLongPtrW(window, GWLP_USERDATA, kHover);
            InvalidateRect(window, nullptr, FALSE);
        }
        break;
    case WM_MOUSELEAVE:
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        InvalidateRect(window, nullptr, FALSE);
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(window, TrackHover, 1);
        break;
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

bool Hovered(const DRAWITEMSTRUCT* draw) {
    return GetWindowLongPtrW(draw->hwndItem, GWLP_USERDATA) == kHover;
}

bool Checked(const DRAWITEMSTRUCT* draw) {
    return SendMessageW(draw->hwndItem, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void DrawLabel(HDC dc, HFONT font, COLORREF color, int left, int top, int width, int height, const wchar_t* text) {
    RECT area{ left, top, left + width, top + height };
    PaintText(dc, font, color, area, text, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
}

int Mix(int value) { return static_cast<int>(std::lround(value)); }
}

void EnableHover(HWND control) {
    if (control) SetWindowSubclass(control, TrackHover, 1, 0);
}

void FillRounded(HDC dc, const RECT& rect, int radius, COLORREF fill, COLORREF border) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius * 2, radius * 2);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

void DrawPrimaryButton(const Theme& theme, const DRAWITEMSTRUCT* draw) {
    const int radius = Dpi(theme, 8);
    RECT rect = draw->rcItem;
    COLORREF fill = (draw->itemState & ODS_SELECTED) ? theme.accentPressed : Hovered(draw) ? theme.accentHover : theme.accent;
    FillRounded(draw->hDC, rect, radius, fill, fill);
    wchar_t label[128]{};
    GetWindowTextW(draw->hwndItem, label, static_cast<int>(std::size(label)));
    PaintText(draw->hDC, theme.semibold, RGB(255, 255, 255), rect, label, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void DrawSecondaryButton(const Theme& theme, const DRAWITEMSTRUCT* draw) {
    const int radius = Dpi(theme, 8);
    RECT rect = draw->rcItem;
    COLORREF fill = (draw->itemState & ODS_SELECTED) ? theme.buttonPressed : Hovered(draw) ? theme.buttonHover : theme.button;
    FillRounded(draw->hDC, rect, radius, fill, theme.border);
    wchar_t label[128]{};
    GetWindowTextW(draw->hwndItem, label, static_cast<int>(std::size(label)));
    PaintText(draw->hDC, theme.semibold, theme.text, rect, label, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void DrawModeCard(const Theme& theme, const DRAWITEMSTRUCT* draw, const wchar_t* title, const wchar_t* detail) {
    const bool selected = Checked(draw);
    RECT rect = draw->rcItem;
    const int radius = Dpi(theme, 10);
    COLORREF fill = selected ? theme.cardSelected : Hovered(draw) ? theme.cardHover : theme.card;
    FillRounded(draw->hDC, rect, radius, fill, selected ? theme.borderSelected : theme.border);

    const int middle = rect.top + (rect.bottom - rect.top) / 2;
    const int ring = Dpi(theme, 9);
    const int centerX = rect.left + Dpi(theme, 30);
    const int border = Dpi(theme, 2);

    HBRUSH ringBrush = CreateSolidBrush(selected ? theme.accent : theme.card);
    HPEN ringPen = CreatePen(PS_SOLID, border, selected ? theme.accent : theme.border);
    HGDIOBJ oldBrush = SelectObject(draw->hDC, ringBrush);
    HGDIOBJ oldPen = SelectObject(draw->hDC, ringPen);
    Ellipse(draw->hDC, centerX - ring, middle - ring, centerX + ring, middle + ring);
    SelectObject(draw->hDC, oldBrush);
    SelectObject(draw->hDC, oldPen);
    DeleteObject(ringBrush);
    DeleteObject(ringPen);

    if (selected) {
        const int dot = Dpi(theme, 4);
        HBRUSH dotBrush = CreateSolidBrush(RGB(255, 255, 255));
        HGDIOBJ previous = SelectObject(draw->hDC, dotBrush);
        HPEN dotPen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
        HGDIOBJ previousPen = SelectObject(draw->hDC, dotPen);
        Ellipse(draw->hDC, centerX - dot, middle - dot, centerX + dot, middle + dot);
        SelectObject(draw->hDC, previous);
        SelectObject(draw->hDC, previousPen);
        DeleteObject(dotBrush);
        DeleteObject(dotPen);
    }

    const int textLeft = rect.left + Dpi(theme, 58);
    const int textWidth = static_cast<int>(rect.right - textLeft - Dpi(theme, 20));
    DrawLabel(draw->hDC, theme.semibold, theme.text, textLeft, rect.top + Dpi(theme, 16), textWidth, Dpi(theme, 22), title);
    DrawLabel(draw->hDC, theme.small, theme.textSecondary, textLeft, rect.top + Dpi(theme, 42), textWidth, Dpi(theme, 20), detail);
}

void DrawToggleCard(const Theme& theme, const DRAWITEMSTRUCT* draw, const wchar_t* title, const wchar_t* detail) {
    const bool enabled = Checked(draw);
    RECT rect = draw->rcItem;
    const int radius = Dpi(theme, 10);
    FillRounded(draw->hDC, rect, radius, Hovered(draw) ? theme.cardHover : theme.card, theme.border);

    const int textLeft = rect.left + Dpi(theme, 24);
    const int textWidth = static_cast<int>(rect.right - textLeft - Dpi(theme, 110));
    DrawLabel(draw->hDC, theme.semibold, theme.text, textLeft, rect.top + Dpi(theme, 16), textWidth, Dpi(theme, 22), title);
    DrawLabel(draw->hDC, theme.small, theme.textSecondary, textLeft, rect.top + Dpi(theme, 42), textWidth, Dpi(theme, 20), detail);

    const int pillWidth = Dpi(theme, 46);
    const int pillHeight = Dpi(theme, 26);
    const int pillLeft = static_cast<int>(rect.right - Dpi(theme, 24) - pillWidth);
    const int middle = rect.top + (rect.bottom - rect.top) / 2;
    const int pillTop = middle - pillHeight / 2;
    RECT pill{ pillLeft, pillTop, pillLeft + pillWidth, pillTop + pillHeight };
    FillRounded(draw->hDC, pill, pillHeight / 2, enabled ? theme.accent : theme.border, enabled ? theme.accent : theme.border);

    const int knob = Dpi(theme, 20);
    const int knobTop = middle - knob / 2;
    const int knobLeft = enabled ? pill.right - Dpi(theme, 3) - knob : pill.left + Dpi(theme, 3);
    HBRUSH brush = CreateSolidBrush(enabled ? RGB(255, 255, 255) : theme.textTertiary);
    HPEN pen = CreatePen(PS_SOLID, 1, enabled ? RGB(255, 255, 255) : theme.textTertiary);
    HGDIOBJ oldBrush = SelectObject(draw->hDC, brush);
    HGDIOBJ oldPen = SelectObject(draw->hDC, pen);
    Ellipse(draw->hDC, knobLeft, knobTop, knobLeft + knob, knobTop + knob);
    SelectObject(draw->hDC, oldBrush);
    SelectObject(draw->hDC, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

void DrawNavItem(const Theme& theme, const DRAWITEMSTRUCT* draw, int icon, bool active) {
    RECT rect = draw->rcItem;
    const bool hover = Hovered(draw);
    if (active || hover)
        FillRounded(draw->hDC, rect, Dpi(theme, 8), active ? theme.navSelected : theme.navHover,
            active ? theme.navSelected : theme.navHover);

    if (active) {
        const int barWidth = Dpi(theme, 3);
        const int barHeight = Dpi(theme, 20);
        RECT bar{ rect.left + Dpi(theme, 4), (rect.top + rect.bottom - barHeight) / 2,
            rect.left + Dpi(theme, 4) + barWidth, (rect.top + rect.bottom + barHeight) / 2 };
        FillRounded(draw->hDC, bar, barWidth / 2, theme.accent, theme.accent);
    }

    const int size = Dpi(theme, 18);
    const int iconLeft = rect.left + Dpi(theme, 18);
    const int iconTop = rect.top + (rect.bottom - rect.top - size) / 2;
    DrawNavIcon(draw->hDC, icon, iconLeft, iconTop, size, active ? theme.accent : theme.textSecondary);

    wchar_t label[128]{};
    GetWindowTextW(draw->hwndItem, label, static_cast<int>(std::size(label)));
    DrawLabel(draw->hDC, active ? theme.semibold : theme.body, active ? theme.accent : theme.text,
        rect.left + Dpi(theme, 46), rect.top, static_cast<int>(rect.right - rect.left - Dpi(theme, 54)),
        static_cast<int>(rect.bottom - rect.top), label);
}

void DrawListRow(const Theme& theme, const DRAWITEMSTRUCT* draw) {
    if (draw->itemID == static_cast<UINT>(-1)) return;
    RECT rect = draw->rcItem;
    if (draw->itemState & ODS_SELECTED) {
        RECT inset = rect;
        InflateRect(&inset, -Dpi(theme, 4), -Dpi(theme, 2));
        FillRounded(draw->hDC, inset, Dpi(theme, 6), theme.navSelected, theme.navSelected);
    }
    wchar_t text[MAX_PATH]{};
    SendMessageW(draw->hwndItem, LB_GETTEXT, draw->itemID, reinterpret_cast<LPARAM>(text));
    DrawLabel(draw->hDC, theme.body, theme.text, static_cast<int>(rect.left + Dpi(theme, 16)), static_cast<int>(rect.top),
        static_cast<int>(rect.right - rect.left - Dpi(theme, 28)), static_cast<int>(rect.bottom - rect.top), text);
}

void DrawStatusDot(HDC dc, int left, int top, int size, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    Ellipse(dc, left, top, left + size, top + size);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

void DrawNavIcon(HDC dc, int icon, int left, int top, int size, COLORREF color) {
    auto point = [&](double x, double y) {
        return POINT{ left + Mix(x * size), top + Mix(y * size) };
    };
    HPEN pen = CreatePen(PS_SOLID, Mix(1.8 * size / 18.0), color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HBRUSH brush = CreateSolidBrush(color);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    SetBkMode(dc, TRANSPARENT);

    switch (icon) {
    case NavPower: {
        POINT bolt[] = { point(0.58, 0.04), point(0.22, 0.55), point(0.47, 0.55), point(0.38, 0.96),
            point(0.79, 0.42), point(0.53, 0.42) };
        Polygon(dc, bolt, static_cast<int>(std::size(bolt)));
        break;
    }
    case NavWhitelist: {
        for (int row = 0; row < 3; ++row) {
            const double y = 0.24 + row * 0.26;
            Ellipse(dc, Mix(0.12 * size), Mix(y * size - 0.05 * size), Mix(0.22 * size), Mix(y * size + 0.05 * size));
            MoveToEx(dc, Mix(0.34 * size), Mix(y * size), nullptr);
            LineTo(dc, Mix(0.9 * size), Mix(y * size));
        }
        break;
    }
    case NavSettings: {
        for (int row = 0; row < 2; ++row) {
            const double y = row == 0 ? 0.32 : 0.7;
            const double knob = row == 0 ? 0.64 : 0.38;
            MoveToEx(dc, Mix(0.1 * size), Mix(y * size), nullptr);
            LineTo(dc, Mix(0.9 * size), Mix(y * size));
            Ellipse(dc, Mix((knob - 0.09) * size), Mix(y * size - 0.09 * size),
                Mix((knob + 0.09) * size), Mix(y * size + 0.09 * size));
        }
        break;
    }
    default: {
        HGDIOBJ outlineBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Ellipse(dc, Mix(0.1 * size), Mix(0.1 * size), Mix(0.9 * size), Mix(0.9 * size));
        SelectObject(dc, outlineBrush);
        Ellipse(dc, Mix(0.44 * size), Mix(0.26 * size), Mix(0.56 * size), Mix(0.38 * size));
        MoveToEx(dc, Mix(0.5 * size), Mix(0.48 * size), nullptr);
        LineTo(dc, Mix(0.5 * size), Mix(0.74 * size));
        break;
    }
    }

    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}
