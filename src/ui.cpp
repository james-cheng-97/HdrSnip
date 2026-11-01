#include "ui.h"

#include <algorithm>
#include <objidl.h>
namespace Gdiplus { using std::min; using std::max; } // gdiplus.h expects min/max; we build with NOMINMAX
#include <gdiplus.h>
#include <dwmapi.h>
#include <string>

namespace ui
{
    namespace
    {
        constexpr wchar_t kButtonClass[] = L"HdrSnipButton";

        Theme MakeTheme(bool dark)
        {
            if (dark)
                return { true, RGB(32, 32, 32), RGB(24, 24, 24), RGB(255, 255, 255), RGB(197, 197, 197),
                         RGB(45, 45, 45), RGB(41, 41, 41), RGB(58, 58, 58),
                         RGB(76, 194, 255), RGB(71, 177, 232), RGB(66, 161, 210), RGB(0, 0, 0) };
            return { false, RGB(243, 243, 243), RGB(232, 232, 232), RGB(26, 26, 26), RGB(93, 93, 93),
                     RGB(233, 233, 233), RGB(237, 237, 237), RGB(229, 229, 229),
                     RGB(0, 95, 184), RGB(25, 110, 191), RGB(51, 131, 205), RGB(255, 255, 255) };
        }

        Theme g_theme = MakeTheme(true);

        bool SystemUsesDarkApps()
        {
            DWORD light = 1, size = sizeof(light);
            RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
            return light == 0;
        }

        Gdiplus::Color ToGdip(COLORREF c) { return Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c)); }

        void FillRounded(HDC dc, RECT r, int radius, COLORREF color)
        {
            Gdiplus::Graphics g(dc);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            const float x = float(r.left), y = float(r.top), w = float(r.right - r.left) - 1, h = float(r.bottom - r.top) - 1;
            const float d = float(radius * 2);
            Gdiplus::GraphicsPath path;
            path.AddArc(x, y, d, d, 180, 90);
            path.AddArc(x + w - d, y, d, d, 270, 90);
            path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
            path.AddArc(x, y + h - d, d, d, 90, 90);
            path.CloseFigure();
            Gdiplus::SolidBrush brush(ToGdip(color));
            g.FillPath(&brush, &path);
        }

        struct Button
        {
            std::wstring glyph, text;
            ButtonStyle style;
            bool hover = false, pressed = false;
        };

        void PaintButton(HWND hwnd, Button& b)
        {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
            HGDIOBJ oldBmp = SelectObject(mem, bmp);

            const Theme& t = g_theme;
            HBRUSH bg = CreateSolidBrush(t.bg);
            FillRect(mem, &rc, bg);
            DeleteObject(bg);

            if (b.style == ButtonStyle::Accent)
                FillRounded(mem, rc, Px(hwnd, 4), b.pressed ? t.accentPressed : b.hover ? t.accentHover : t.accent);
            else if (b.pressed || b.hover)
                FillRounded(mem, rc, Px(hwnd, 4), b.pressed ? t.pressed : t.hover);

            HFONT icon = IconFont(hwnd, 10), font = TextFont(hwnd, 10, b.style == ButtonStyle::Accent ? FW_SEMIBOLD : FW_NORMAL);
            const COLORREF fg = b.style == ButtonStyle::Accent ? t.accentText : t.text;
            SetBkMode(mem, TRANSPARENT);
            SetTextColor(mem, fg);

            SIZE gs{}, ts{};
            SelectObject(mem, icon);
            GetTextExtentPoint32W(mem, b.glyph.c_str(), int(b.glyph.size()), &gs);
            SelectObject(mem, font);
            GetTextExtentPoint32W(mem, b.text.c_str(), int(b.text.size()), &ts);
            const int gap = b.text.empty() ? 0 : Px(hwnd, 8);
            int x = (rc.right - (gs.cx + gap + ts.cx)) / 2;

            RECT gr{ x, 0, x + gs.cx, rc.bottom };
            SelectObject(mem, icon);
            DrawTextW(mem, b.glyph.c_str(), -1, &gr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            RECT tr{ x + gs.cx + gap, 0, rc.right, rc.bottom };
            SelectObject(mem, font);
            DrawTextW(mem, b.text.c_str(), -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);

            BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, oldBmp);
            DeleteObject(bmp);
            DeleteDC(mem);
            DeleteObject(icon);
            DeleteObject(font);
            EndPaint(hwnd, &ps);
        }

        LRESULT CALLBACK ButtonProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
        {
            auto b = reinterpret_cast<Button*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (msg == WM_NCCREATE)
            {
                b = static_cast<Button*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(b));
            }
            if (!b) return DefWindowProcW(hwnd, msg, wp, lp);

            switch (msg)
            {
            case WM_PAINT:
                PaintButton(hwnd, *b);
                return 0;
            case WM_ERASEBKGND:
                return 1;
            case WM_MOUSEMOVE:
                if (!b->hover)
                {
                    b->hover = true;
                    TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
                    TrackMouseEvent(&tme);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            case WM_MOUSELEAVE:
                b->hover = false;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            case WM_LBUTTONDOWN:
                b->pressed = true;
                SetCapture(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            case WM_LBUTTONUP:
                if (b->pressed)
                {
                    b->pressed = false;
                    ReleaseCapture();
                    InvalidateRect(hwnd, nullptr, FALSE);
                    RECT rc;
                    GetClientRect(hwnd, &rc);
                    POINT pt{ short(LOWORD(lp)), short(HIWORD(lp)) };
                    // Last thing we do: the parent may destroy this button in response.
                    if (PtInRect(&rc, pt))
                        SendMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(hwnd), BN_CLICKED), LPARAM(hwnd));
                }
                return 0;
            case WM_SETCURSOR:
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            case WM_NCDESTROY:
                delete b;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                break;
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
    }

    void Init()
    {
        static ULONG_PTR token = 0;
        Gdiplus::GdiplusStartupInput input;
        Gdiplus::GdiplusStartup(&token, &input, nullptr);

        WNDCLASSW wc{};
        wc.lpfnWndProc = ButtonProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kButtonClass;
        RegisterClassW(&wc);
        Refresh();
    }

    const Theme& Current() { return g_theme; }

    void Refresh() { g_theme = MakeTheme(SystemUsesDarkApps()); }

    void ApplyChrome(HWND hwnd)
    {
        BOOL dark = g_theme.dark;
        DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        COLORREF caption = g_theme.bg;
        DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
        COLORREF border = DWMWA_COLOR_DEFAULT;
        DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));
    }

    int Px(HWND hwnd, int dip) { return MulDiv(dip, GetDpiForWindow(hwnd), 96); }

    HFONT TextFont(HWND hwnd, int pt, int weight)
    {
        return CreateFontW(-MulDiv(pt, GetDpiForWindow(hwnd), 72), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
            CLEARTYPE_QUALITY, 0, L"Segoe UI Variable Text");
    }

    HFONT IconFont(HWND hwnd, int pt)
    {
        return CreateFontW(-MulDiv(pt, GetDpiForWindow(hwnd), 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
            CLEARTYPE_QUALITY, 0, L"Segoe Fluent Icons");
    }

    void DrawLabel(HDC dc, RECT r, const wchar_t* text, HFONT font, COLORREF color, UINT flags)
    {
        HGDIOBJ old = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, color);
        DrawTextW(dc, text, -1, &r, flags | DT_NOPREFIX);
        SelectObject(dc, old);
    }

    HWND CreateButton(HWND parent, int id, const wchar_t* glyph, const wchar_t* text, ButtonStyle style)
    {
        auto b = new Button{ glyph, text, style };
        HWND hwnd = CreateWindowExW(0, kButtonClass, text, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, parent,
            HMENU(INT_PTR(id)), GetModuleHandleW(nullptr), b);
        if (!hwnd) delete b;
        return hwnd;
    }

    void SetButtonText(HWND button, const wchar_t* text)
    {
        auto b = reinterpret_cast<Button*>(GetWindowLongPtrW(button, GWLP_USERDATA));
        if (!b) return;
        b->text = text;
        SetWindowTextW(button, text);
        InvalidateRect(button, nullptr, FALSE);
    }
}
