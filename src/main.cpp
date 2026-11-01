#include "capture.h"
#include "output.h"
#include "overlay.h"
#include "preview.h"
#include "share.h"
#include "tonemap.h"
#include "ui.h"

#include <winrt/base.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <cstring>
#include <memory>
#include <optional>
#include <string>

namespace
{
    enum HotkeyId { kSnip = 1, kQuit = 2 };
    constexpr ULONG_PTR kCopyStream = 0x48445253; // WM_COPYDATA tag: "--stream" from a second launch

    void ShowError(const std::wstring& text)
    {
        MessageBoxW(nullptr, text.c_str(), L"HdrSnip", MB_ICONERROR | MB_OK);
    }

    SnipImage Crop(const HdrFrame& frame, const std::vector<uint8_t>& sdr, float sdrWhiteNits, const RECT& r)
    {
        SnipImage img;
        img.sdrWhiteNits = sdrWhiteNits;
        img.width = r.right - r.left;
        img.height = r.bottom - r.top;
        img.sdrBgra.resize(size_t(img.width) * img.height * 4);
        img.hdrRgba.resize(size_t(img.width) * img.height * 4);
        for (int y = 0; y < img.height; ++y)
        {
            const size_t src = (size_t(r.top + y) * frame.width + r.left) * 4, dst = size_t(y) * img.width * 4;
            std::memcpy(&img.sdrBgra[dst], &sdr[src], size_t(img.width) * 4);
            std::memcpy(&img.hdrRgba[dst], &frame.rgba[src], size_t(img.width) * 4 * sizeof(float));
        }
        return img;
    }

    // Capture the monitor under the cursor and crop to the user's selection (or everything if fullScreen).
    std::optional<SnipImage> Snip(HMONITOR monitor, bool fullScreen)
    {
        HdrFrame frame = CaptureMonitor(monitor);
        const float sdrWhite = GetSdrWhiteNits(monitor);
        std::vector<uint8_t> sdr = ToneMapToBgra8(frame, sdrWhite);

        RECT r{ 0, 0, frame.width, frame.height };
        if (!fullScreen)
        {
            auto selected = SelectRegion(monitor, sdr.data(), frame.width, frame.height);
            if (!selected) return std::nullopt;
            r = *selected;
        }
        return Crop(frame, sdr, sdrWhite, r);
    }

    // Interactive snip: like Snipping Tool, copy to clipboard and open a preview; nothing is saved
    // until the user picks Save as. Headless (--fullscreen) instead writes PNG + JXR + Ultra HDR JPEG to Pictures\HdrSnip.
    bool RunSnip(bool headless)
    {
        try
        {
            POINT cursor;
            GetCursorPos(&cursor);
            HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);

            auto img = Snip(monitor, headless);
            if (!img) return true;
            CopyBgraToClipboard(img->sdrBgra.data(), img->width, img->height, img->width);
            if (headless)
            {
                auto base = NextOutputBasePath();
                SavePng(std::filesystem::path(base).concat(L".png"), img->sdrBgra.data(), img->width, img->height, img->width);
                SaveJxr(std::filesystem::path(base).concat(L".jxr"), img->hdrRgba.data(), img->width, img->height, img->width);
                SaveUltraHdrJpeg(std::filesystem::path(base).concat(L".jpg"), img->sdrBgra.data(), img->hdrRgba.data(),
                    img->width, img->height, img->width, img->sdrWhiteNits);
            }
            else
            {
                ShowPreview(monitor, std::move(*img));
            }
            return true;
        }
        catch (const winrt::hresult_error& e)
        {
            ShowError(L"Snip failed: " + std::wstring(e.message()));
        }
        catch (const std::exception& e)
        {
            ShowError(L"Snip failed: " + std::wstring(e.what(), e.what() + strlen(e.what())));
        }
        return false;
    }

    // --- Launcher window: Snipping-Tool-style "+ New snip" bar. Closing it quits the app. ---

    constexpr int kNewSnip = 201, kStream = 202, kDelay = 203;
    constexpr UINT_PTR kDelayTimer = 1;
    constexpr int kDelayChoices[] = { 0, 3, 5, 10 };
    constexpr wchar_t kSettingsKey[] = L"Software\\HdrSnip";

    void LayoutLauncher(HWND hwnd, BOOL repaint)
    {
        const int y = ui::Px(hwnd, 12), h = ui::Px(hwnd, 36);
        MoveWindow(GetDlgItem(hwnd, kNewSnip), ui::Px(hwnd, 12), y, ui::Px(hwnd, 92), h, repaint);
        MoveWindow(GetDlgItem(hwnd, kDelay), ui::Px(hwnd, 12 + 92 + 4), y, ui::Px(hwnd, 120), h, repaint);
        MoveWindow(GetDlgItem(hwnd, kStream), ui::Px(hwnd, 12 + 92 + 4 + 120 + 4), y, ui::Px(hwnd, 104), h, repaint);
    }
    HWND g_main = nullptr;
    int g_delaySeconds = 0;     // Snipping-Tool-style delay for New (the Ctrl+Alt+S hotkey is always instant)
    bool g_delayPending = false;

    std::wstring DelayLabel(int seconds)
    {
        return seconds == 0 ? L"No delay" : std::to_wstring(seconds) + L"-second delay";
    }

    // Remembered across runs, like Snipping Tool.
    int LoadDelay()
    {
        DWORD value = 0, size = sizeof(value);
        RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"DelaySeconds", RRF_RT_REG_DWORD, nullptr, &value, &size);
        for (int c : kDelayChoices) if (int(value) == c) return c;
        return 0;
    }

    void SaveDelay(int seconds)
    {
        DWORD value = DWORD(seconds);
        RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"DelaySeconds", REG_DWORD, &value, sizeof(value));
    }

    void SnipFromLauncher()
    {
        // Hide instantly (no fade animation) so the launcher isn't in the capture.
        if (g_main) { ShowWindow(g_main, SW_HIDE); Sleep(80); }
        RunSnip(false);
        if (g_main) ShowWindow(g_main, OpenPreviewCount() > 0 ? SW_SHOWNOACTIVATE : SW_SHOW);
    }

    // New button (launcher or preview): waits out the delay with the launcher hidden, so you can
    // open menus or tooltips before the screen freezes.
    void NewSnipWithDelay()
    {
        if (g_delayPending) return;
        if (!g_main || g_delaySeconds == 0) { SnipFromLauncher(); return; }
        g_delayPending = true;
        ShowWindow(g_main, SW_HIDE);
        SetTimer(g_main, kDelayTimer, UINT(g_delaySeconds) * 1000, nullptr);
    }

    void ChooseDelay(HWND hwnd)
    {
        HMENU menu = CreatePopupMenu();
        for (int i = 0; i < int(std::size(kDelayChoices)); ++i)
            AppendMenuW(menu, MF_STRING | (kDelayChoices[i] == g_delaySeconds ? MF_CHECKED : 0), i + 1,
                DelayLabel(kDelayChoices[i]).c_str());
        RECT r;
        GetWindowRect(GetDlgItem(hwnd, kDelay), &r);
        const UINT id = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, r.left, r.bottom, 0, hwnd, nullptr);
        DestroyMenu(menu);
        if (id == 0) return;
        g_delaySeconds = kDelayChoices[id - 1];
        SaveDelay(g_delaySeconds);
        ui::SetButtonText(GetDlgItem(hwnd, kDelay), DelayLabel(g_delaySeconds).c_str());
    }

    void PaintLauncher(HWND hwnd)
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);

        const ui::Theme& t = ui::Current();
        HBRUSH bg = CreateSolidBrush(t.bg);
        FillRect(mem, &rc, bg);
        DeleteObject(bg);

        // Divider under the toolbar, then the centered hint like Snipping Tool's empty state.
        RECT line{ 0, ui::Px(hwnd, 60), rc.right, ui::Px(hwnd, 61) };
        HBRUSH border = CreateSolidBrush(t.border);
        FillRect(mem, &line, border);
        DeleteObject(border);

        HFONT body = ui::TextFont(hwnd, 11), note = ui::TextFont(hwnd, 9);
        const int mid = (line.bottom + rc.bottom) / 2;
        ui::DrawLabel(mem, { 0, mid - ui::Px(hwnd, 28), rc.right, mid + ui::Px(hwnd, 2) },
            L"Select New or press Ctrl+Alt+S to start a snip.", body, t.text, DT_CENTER | DT_SINGLELINE | DT_BOTTOM);
        ui::DrawLabel(mem, { 0, mid + ui::Px(hwnd, 6), rc.right, mid + ui::Px(hwnd, 30) },
            L"Stream opens an HDR-corrected window to share in Discord (Go Live → Applications).",
            note, t.subtext, DT_CENTER | DT_SINGLELINE | DT_TOP);
        DeleteObject(body);
        DeleteObject(note);

        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
    }

    LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg)
        {
        case WM_CREATE:
        {
            ui::CreateButton(hwnd, kNewSnip, ui::kGlyphAdd, L"New", ui::ButtonStyle::Accent);
            ui::CreateButton(hwnd, kDelay, ui::kGlyphDelay, DelayLabel(g_delaySeconds).c_str(), ui::ButtonStyle::Subtle);
            ui::CreateButton(hwnd, kStream, ui::kGlyphStream, L"Stream", ui::ButtonStyle::Subtle);
            LayoutLauncher(hwnd, FALSE);
            ui::ApplyChrome(hwnd);
            BOOL disable = TRUE;
            DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &disable, sizeof(disable));
            return 0;
        }
        case WM_DPICHANGED:
        {
            auto r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER);
            LayoutLauncher(hwnd, TRUE);
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
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            PaintLauncher(hwnd);
            return 0;
        case WM_COMMAND:
            if (LOWORD(wp) == kNewSnip) NewSnipWithDelay();
            if (LOWORD(wp) == kDelay) ChooseDelay(hwnd);
            if (LOWORD(wp) == kStream) ChooseStreamSource(hwnd);
            return 0;
        case WM_TIMER:
            if (wp == kDelayTimer)
            {
                KillTimer(hwnd, kDelayTimer);
                g_delayPending = false;
                SnipFromLauncher();
            }
            return 0;
        case WM_COPYDATA:
        {
            auto cds = reinterpret_cast<const COPYDATASTRUCT*>(lp);
            if (cds->dwData != kCopyStream || cds->cbData < sizeof(wchar_t)) return FALSE;
            // Copy out first: the sender's buffer is only valid during this call.
            std::wstring title(static_cast<const wchar_t*>(cds->lpData), cds->cbData / sizeof(wchar_t) - 1);
            PostMessageW(hwnd, WM_APP, 0, reinterpret_cast<LPARAM>(new std::wstring(std::move(title))));
            return TRUE;
        }
        case WM_APP:
        {
            std::unique_ptr<std::wstring> title(reinterpret_cast<std::wstring*>(lp));
            OpenShareWindow(title->c_str());
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void CreateLauncher()
    {
        WNDCLASSW wc{};
        wc.lpfnWndProc = MainProc;
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
        wc.lpszClassName = L"HdrSnipMain";
        RegisterClassW(&wc);

        // Size the client area at the DPI of the monitor the window will open on.
        POINT cursor;
        GetCursorPos(&cursor);
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &mi);
        const UINT dpi = GetDpiForSystem();
        RECT r{ 0, 0, MulDiv(560, dpi, 96), MulDiv(170, dpi, 96) };
        const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi);
        const int w = r.right - r.left, h = r.bottom - r.top;
        g_main = CreateWindowExW(0, wc.lpszClassName, L"HdrSnip", style,
            (mi.rcWork.left + mi.rcWork.right - w) / 2, mi.rcWork.top + MulDiv(80, dpi, 96), w, h,
            nullptr, nullptr, wc.hInstance, nullptr);
        ShowWindow(g_main, SW_SHOW);
    }

    void RunMessageLoop(bool exitWhenNoPreviews)
    {
        MSG msg;
        while ((!exitWhenNoPreviews || OpenPreviewCount() > 0) && GetMessageW(&msg, nullptr, 0, 0) > 0)
        {
            if (msg.message == WM_HOTKEY && msg.wParam == kSnip) SnipFromLauncher();
            else if (msg.message == WM_HOTKEY && msg.wParam == kQuit) break;
            if (g_main && msg.message == WM_KEYDOWN && msg.wParam == 'N' && GetKeyState(VK_CONTROL) < 0 &&
                GetAncestor(msg.hwnd, GA_ROOT) == g_main)
            {
                SnipFromLauncher();
                continue;
            }
            if (HandlePreviewKey(msg)) continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    ui::Init();
    SetNewSnipHandler(NewSnipWithDelay);
    g_delaySeconds = LoadDelay();

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring arg = argc > 1 ? argv[1] : L"";
    std::wstring arg2 = argc > 2 ? argv[2] : L"";
    LocalFree(argv);

    // One-shot modes, handy for scripting and for testing without the hotkey loop.
    if (arg == L"--fullscreen") return RunSnip(true) ? 0 : 1;
    if (arg == L"--snip")
    {
        if (!RunSnip(false)) return 1;
        RunMessageLoop(true);
        return 0;
    }

    // A second launch just brings the existing window forward.
    if (HWND existing = FindWindowW(L"HdrSnipMain", nullptr))
    {
        if (arg == L"--stream") // hand the request to the running instance
        {
            COPYDATASTRUCT cds{ kCopyStream, DWORD((arg2.size() + 1) * sizeof(wchar_t)), arg2.data() };
            SendMessageW(existing, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds));
            return 0;
        }
        ShowWindow(existing, SW_RESTORE);
        SetForegroundWindow(existing);
        return 0;
    }

    // The hotkey is a bonus; the launcher still works if another app owns it.
    RegisterHotKey(nullptr, kSnip, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'S');
    RegisterHotKey(nullptr, kQuit, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'Q');
    CreateLauncher();
    if (arg == L"--stream") OpenShareWindow(arg2.c_str()); // optional: part of a window title
    RunMessageLoop(false);
    UnregisterHotKey(nullptr, kSnip);
    UnregisterHotKey(nullptr, kQuit);
    return 0;
}
