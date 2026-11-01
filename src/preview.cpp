#include "preview.h"
#include "output.h"
#include "ui.h"

#include <shobjidl.h>
#include <winrt/base.h>
#include <algorithm>
#include <cstring>
#include <string>

namespace
{
    constexpr wchar_t kClass[] = L"HdrSnipPreview";
    enum Command { kSave = 101, kCopy = 102, kNew = 103 };
    int g_open = 0;
    void (*g_newSnip)() = nullptr;

    struct Preview
    {
        SnipImage image;
        HBITMAP bitmap = nullptr;
    };

    int ToolbarHeight(HWND hwnd) { return ui::Px(hwnd, 60); }

    void SaveAs(HWND hwnd, const Preview& p)
    {
        auto dialog = winrt::create_instance<IFileSaveDialog>(CLSID_FileSaveDialog);
        const COMDLG_FILTERSPEC types[] = {
            { L"PNG (SDR, opens everywhere)", L"*.png" },
            { L"JPEG Ultra HDR (HDR where supported, SDR elsewhere)", L"*.jpg" },
            { L"JPEG XR (full HDR)", L"*.jxr" },
        };
        dialog->SetFileTypes(ARRAYSIZE(types), types);
        dialog->SetFileTypeIndex(1);
        dialog->SetDefaultExtension(L"png");

        auto base = NextOutputBasePath();
        winrt::com_ptr<IShellItem> folder;
        if (SUCCEEDED(SHCreateItemFromParsingName(base.parent_path().c_str(), nullptr, IID_PPV_ARGS(folder.put()))))
            dialog->SetFolder(folder.get());
        dialog->SetFileName(base.filename().c_str());

        if (dialog->Show(hwnd) != S_OK) return; // cancelled
        winrt::com_ptr<IShellItem> item;
        winrt::check_hresult(dialog->GetResult(item.put()));
        PWSTR raw = nullptr;
        winrt::check_hresult(item->GetDisplayName(SIGDN_FILESYSPATH, &raw));
        std::filesystem::path path(raw);
        CoTaskMemFree(raw);

        const auto& img = p.image;
        const auto ext = path.extension().wstring();
        if (_wcsicmp(ext.c_str(), L".jxr") == 0)
            SaveJxr(path, img.hdrRgba.data(), img.width, img.height, img.width);
        else if (_wcsicmp(ext.c_str(), L".jpg") == 0 || _wcsicmp(ext.c_str(), L".jpeg") == 0)
            SaveUltraHdrJpeg(path, img.sdrBgra.data(), img.hdrRgba.data(), img.width, img.height, img.width,
                img.sdrWhiteNits);
        else
            SavePng(path, img.sdrBgra.data(), img.width, img.height, img.width);
    }

    void Paint(HWND hwnd, const Preview& p)
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client;
        GetClientRect(hwnd, &client);
        const int top = ToolbarHeight(hwnd);
        const ui::Theme& t = ui::Current();

        HDC back = CreateCompatibleDC(dc);
        HBITMAP backBmp = CreateCompatibleBitmap(dc, client.right, client.bottom);
        HGDIOBJ oldBack = SelectObject(back, backBmp);
        HBRUSH bg = CreateSolidBrush(t.bg), canvas = CreateSolidBrush(t.canvas), border = CreateSolidBrush(t.border);
        FillRect(back, &client, bg);
        RECT area{ 0, top, client.right, client.bottom };
        FillRect(back, &area, canvas);
        RECT divider{ 0, top - 1, client.right, top };
        FillRect(back, &divider, border);

        // Image size in the middle of the toolbar.
        HFONT note = ui::TextFont(hwnd, 9);
        const std::wstring dims = std::to_wstring(p.image.width) + L" × " + std::to_wstring(p.image.height);
        ui::DrawLabel(back, { 0, 0, client.right, top - 1 }, dims.c_str(), note, t.subtext,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        DeleteObject(note);

        // Fit inside the canvas with a margin; never upscale past 1:1.
        const int margin = ui::Px(hwnd, 20);
        const int availW = std::max(1, int(area.right - 2 * margin));
        const int availH = std::max(1, int(area.bottom - area.top - 2 * margin));
        const double s = std::min({ 1.0, double(availW) / p.image.width, double(availH) / p.image.height });
        const int w = std::max(1, int(p.image.width * s)), h = std::max(1, int(p.image.height * s));
        const int x = (area.right - w) / 2, y = area.top + (area.bottom - area.top - h) / 2;

        RECT frame{ x - 1, y - 1, x + w + 1, y + h + 1 };
        FillRect(back, &frame, border);
        HDC src = CreateCompatibleDC(dc);
        HGDIOBJ oldSrc = SelectObject(src, p.bitmap);
        SetStretchBltMode(back, HALFTONE);
        SetBrushOrgEx(back, 0, 0, nullptr);
        StretchBlt(back, x, y, w, h, src, 0, 0, p.image.width, p.image.height, SRCCOPY);
        SelectObject(src, oldSrc);
        DeleteDC(src);

        BitBlt(dc, 0, 0, client.right, client.bottom, back, 0, 0, SRCCOPY);
        DeleteObject(bg);
        DeleteObject(canvas);
        DeleteObject(border);
        SelectObject(back, oldBack);
        DeleteObject(backBmp);
        DeleteDC(back);
        EndPaint(hwnd, &ps);
    }

    void LayoutButtons(HWND hwnd)
    {
        RECT client;
        GetClientRect(hwnd, &client);
        const int pad = ui::Px(hwnd, 12), h = ui::Px(hwnd, 36), y = (ToolbarHeight(hwnd) - h) / 2;
        const int bw = ui::Px(hwnd, 92), gap = ui::Px(hwnd, 4);
        MoveWindow(GetDlgItem(hwnd, kNew), pad, y, bw, h, TRUE);
        MoveWindow(GetDlgItem(hwnd, kSave), client.right - pad - bw, y, bw, h, TRUE);
        MoveWindow(GetDlgItem(hwnd, kCopy), client.right - pad - 2 * bw - gap, y, bw, h, TRUE);
    }

    LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        auto p = reinterpret_cast<Preview*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE)
        {
            p = static_cast<Preview*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(p));
        }
        if (!p) return DefWindowProcW(hwnd, msg, wp, lp);

        switch (msg)
        {
        case WM_CREATE:
            ui::CreateButton(hwnd, kNew, ui::kGlyphAdd, L"New", ui::ButtonStyle::Accent);
            ui::CreateButton(hwnd, kCopy, ui::kGlyphCopy, L"Copy", ui::ButtonStyle::Subtle);
            ui::CreateButton(hwnd, kSave, ui::kGlyphSave, L"Save", ui::ButtonStyle::Subtle);
            ui::ApplyChrome(hwnd);
            LayoutButtons(hwnd);
            return 0;
        case WM_DPICHANGED:
        {
            auto r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER);
            return 0;
        }
        case WM_SETTINGCHANGE:
            if (lp && wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0)
            {
                ui::Refresh();
                ui::ApplyChrome(hwnd);
                RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
            }
            return 0;
        case WM_SIZE:
            LayoutButtons(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = { ui::Px(hwnd, 360), ui::Px(hwnd, 200) };
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            Paint(hwnd, *p);
            return 0;
        case WM_COMMAND:
            if (LOWORD(wp) == kNew)
            {
                // Like Snipping Tool: the new snip replaces this one.
                auto handler = g_newSnip;
                DestroyWindow(hwnd);
                if (handler) handler();
                return 0;
            }
            try
            {
                if (LOWORD(wp) == kSave) SaveAs(hwnd, *p);
                if (LOWORD(wp) == kCopy)
                    CopyBgraToClipboard(p->image.sdrBgra.data(), p->image.width, p->image.height, p->image.width);
            }
            catch (const winrt::hresult_error& e)
            {
                MessageBoxW(hwnd, (L"Failed: " + std::wstring(e.message())).c_str(), L"HdrSnip", MB_ICONERROR);
            }
            catch (const std::exception&)
            {
                MessageBoxW(hwnd, L"Failed to write the file.", L"HdrSnip", MB_ICONERROR);
            }
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) DestroyWindow(hwnd);
            return 0;
        case WM_NCDESTROY:
            DeleteObject(p->bitmap);
            delete p;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            --g_open;
            PostMessageW(nullptr, WM_NULL, 0, 0); // wake the main loop so one-shot mode can exit
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

void SetNewSnipHandler(void (*handler)()) { g_newSnip = handler; }

void ShowPreview(HMONITOR monitor, SnipImage image)
{
    static bool registered = [] {
        WNDCLASSW wc{};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
        wc.lpszClassName = kClass;
        return RegisterClassW(&wc) != 0;
    }();
    if (!registered) return;

    auto p = new Preview{ std::move(image) };
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), p->image.width, -p->image.height, 1, 32, BI_RGB };
    void* bits = nullptr;
    p->bitmap = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (p->bitmap) std::memcpy(bits, p->image.sdrBgra.data(), p->image.sdrBgra.size());

    // Start at image size (plus chrome), capped to 80% of the monitor's work area, centered.
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(monitor, &mi);
    UINT dpiX = 96, dpiY = 96;
    if (HMODULE shcore = LoadLibraryW(L"shcore.dll"))
    {
        using Fn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
        if (auto f = reinterpret_cast<Fn>(GetProcAddress(shcore, "GetDpiForMonitor"))) f(monitor, 0, &dpiX, &dpiY);
        FreeLibrary(shcore);
    }
    const RECT work = mi.rcWork;
    const int workW = work.right - work.left, workH = work.bottom - work.top;
    RECT r{ 0, 0, std::max(p->image.width + MulDiv(40, dpiX, 96), MulDiv(480, dpiX, 96)),
            p->image.height + MulDiv(60 + 40, dpiX, 96) };
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpiX);
    const int w = std::min<int>(r.right - r.left, workW * 8 / 10);
    const int h = std::min<int>(r.bottom - r.top, workH * 8 / 10);

    HWND hwnd = CreateWindowExW(0, kClass, L"HdrSnip", WS_OVERLAPPEDWINDOW,
        work.left + (workW - w) / 2, work.top + (workH - h) / 2, w, h,
        nullptr, nullptr, GetModuleHandleW(nullptr), p);
    if (!hwnd) { DeleteObject(p->bitmap); delete p; return; }
    ++g_open;
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
}

int OpenPreviewCount() { return g_open; }

bool HandlePreviewKey(const MSG& msg)
{
    if (msg.message != WM_KEYDOWN || !msg.hwnd) return false;
    HWND root = GetAncestor(msg.hwnd, GA_ROOT);
    wchar_t cls[32];
    if (!GetClassNameW(root, cls, ARRAYSIZE(cls)) || wcscmp(cls, kClass) != 0) return false;

    const bool ctrl = GetKeyState(VK_CONTROL) < 0;
    if (ctrl && msg.wParam == 'S') { SendMessageW(root, WM_COMMAND, kSave, 0); return true; }
    if (ctrl && msg.wParam == 'C') { SendMessageW(root, WM_COMMAND, kCopy, 0); return true; }
    if (ctrl && msg.wParam == 'N') { SendMessageW(root, WM_COMMAND, kNew, 0); return true; }
    if (msg.wParam == VK_ESCAPE) { DestroyWindow(root); return true; }
    return false;
}
