#include "overlay.h"

#include <windowsx.h>
#include <algorithm>
#include <cstring>
#include <vector>

namespace
{
    struct Overlay
    {
        int width = 0, height = 0;
        HBITMAP bright = nullptr, dim = nullptr;
        bool dragging = false, done = false;
        POINT start{}, current{};
        std::optional<RECT> result;

        RECT Selection() const
        {
            return { std::min(start.x, current.x), std::min(start.y, current.y),
                     std::max(start.x, current.x), std::max(start.y, current.y) };
        }
    };

    HBITMAP MakeDib(const uint8_t* bgra, int w, int h)
    {
        BITMAPINFO bi{};
        bi.bmiHeader = { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB };
        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bmp) std::memcpy(bits, bgra, size_t(w) * h * 4);
        return bmp;
    }

    void Paint(HWND hwnd, Overlay& o)
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        HDC back = CreateCompatibleDC(dc);
        HBITMAP backBmp = CreateCompatibleBitmap(dc, o.width, o.height);
        HGDIOBJ oldBack = SelectObject(back, backBmp);
        HDC src = CreateCompatibleDC(dc);

        HGDIOBJ oldSrc = SelectObject(src, o.dim);
        BitBlt(back, 0, 0, o.width, o.height, src, 0, 0, SRCCOPY);
        if (o.dragging)
        {
            RECT r = o.Selection();
            SelectObject(src, o.bright);
            BitBlt(back, r.left, r.top, r.right - r.left, r.bottom - r.top, src, r.left, r.top, SRCCOPY);
            HBRUSH frame = CreateSolidBrush(RGB(0, 120, 215));
            FrameRect(back, &r, frame);
            DeleteObject(frame);
        }
        BitBlt(dc, 0, 0, o.width, o.height, back, 0, 0, SRCCOPY);

        SelectObject(src, oldSrc);
        DeleteDC(src);
        SelectObject(back, oldBack);
        DeleteObject(backBmp);
        DeleteDC(back);
        EndPaint(hwnd, &ps);
    }

    LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        auto o = reinterpret_cast<Overlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!o) return DefWindowProcW(hwnd, msg, wp, lp);

        switch (msg)
        {
        case WM_PAINT:
            Paint(hwnd, *o);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_LBUTTONDOWN:
            o->dragging = true;
            o->start = o->current = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            SetCapture(hwnd);
            return 0;
        case WM_MOUSEMOVE:
            if (o->dragging)
            {
                o->current = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONUP:
            if (o->dragging)
            {
                ReleaseCapture();
                o->current = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
                RECT r = o->Selection();
                r.left = std::clamp<LONG>(r.left, 0, o->width);
                r.right = std::clamp<LONG>(r.right, 0, o->width);
                r.top = std::clamp<LONG>(r.top, 0, o->height);
                r.bottom = std::clamp<LONG>(r.bottom, 0, o->height);
                if (r.right - r.left < 4 || r.bottom - r.top < 4) r = { 0, 0, o->width, o->height };
                o->result = r;
                o->done = true;
            }
            return 0;
        case WM_RBUTTONUP:
            o->done = true;
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) o->done = true;
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

std::optional<RECT> SelectRegion(HMONITOR monitor, const uint8_t* bgra, int width, int height)
{
    static const wchar_t* kClass = L"HdrSnipOverlay";
    static bool registered = [] {
        WNDCLASSW wc{};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
        wc.lpszClassName = kClass;
        return RegisterClassW(&wc) != 0;
    }();
    if (!registered) return std::nullopt;

    Overlay o;
    o.width = width;
    o.height = height;
    o.bright = MakeDib(bgra, width, height);
    std::vector<uint8_t> darker(bgra, bgra + size_t(width) * height * 4);
    for (size_t i = 0; i < darker.size(); ++i)
        if (i % 4 != 3) darker[i] = uint8_t(darker[i] / 2);
    o.dim = MakeDib(darker.data(), width, height);

    // Per-monitor-v2 DPI awareness makes rcMonitor physical pixels, matching the capture size.
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(monitor, &mi);
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kClass, L"HdrSnip", WS_POPUP,
        mi.rcMonitor.left, mi.rcMonitor.top, width, height, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&o));
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);

    MSG msg;
    while (!o.done && GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DestroyWindow(hwnd);
    DeleteObject(o.bright);
    DeleteObject(o.dim);
    return o.result;
}
