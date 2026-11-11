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
#include <commctrl.h>
#include <cstring>
#include <memory>
#include <optional>
#include <string>

namespace
{
    enum HotkeyId { kSnip = 1, kQuit = 2, kSnipPrintScreen = 3 };
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

    // --- Launcher window: Snipping-Tool-style "+ New snip" bar. Closing it hides HdrSnip to the
    //     notification area (tray) so the hotkeys keep working; Quit is in the tray menu. ---

    constexpr int kNewSnip = 201, kStream = 202, kDelay = 203;
    constexpr UINT_PTR kDelayTimer = 1;
    constexpr int kDelayChoices[] = { 0, 3, 5, 10 };
    constexpr wchar_t kSettingsKey[] = L"Software\\HdrSnip";
    constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    constexpr UINT kTrayMessage = WM_APP + 1;
    constexpr UINT kPrintScreenPressed = WM_APP + 2;
    enum TrayCommand { kTraySnip = 1, kTrayStream, kTrayOpen, kTrayQuit };
    constexpr int kSettings = 204;
    HWND g_main = nullptr;
    bool g_printScreen = false; // whether we own the Print Screen key
    UINT g_taskbarCreated = 0;  // Explorer restarted: the tray icon must be re-added

    bool ReadFlag(const wchar_t* name)
    {
        DWORD value = 0, size = sizeof(value);
        RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_DWORD, nullptr, &value, &size);
        return value != 0;
    }

    void WriteFlag(const wchar_t* name, bool on)
    {
        DWORD value = on;
        RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, name, REG_DWORD, &value, sizeof(value));
    }

    // Start with Windows = an HKCU Run entry that launches straight into the tray.
    std::wstring AutostartCommand()
    {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        return L"\"" + std::wstring(exe) + L"\" --tray";
    }

    bool IsAutostartOn()
    {
        wchar_t value[MAX_PATH + 32] = {};
        DWORD size = sizeof(value);
        if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, L"HdrSnip", RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS)
            return false;
        return _wcsicmp(value, AutostartCommand().c_str()) == 0; // stale path (exe moved) counts as off
    }

    void SetAutostart(bool on)
    {
        if (on)
        {
            const std::wstring cmd = AutostartCommand();
            RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, L"HdrSnip", REG_SZ, cmd.c_str(), DWORD((cmd.size() + 1) * sizeof(wchar_t)));
        }
        else
        {
            RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, L"HdrSnip");
        }
    }

    void AddTrayIcon(HWND hwnd)
    {
        NOTIFYICONDATAW nid{ sizeof(nid) };
        nid.hWnd = hwnd;
        nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        nid.uCallbackMessage = kTrayMessage;
        nid.hIcon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
        wcscpy_s(nid.szTip, g_printScreen ? L"HdrSnip — Print Screen to snip" : L"HdrSnip — Ctrl+Alt+S to snip");
        Shell_NotifyIconW(NIM_ADD, &nid);
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
    }

    void RemoveTrayIcon(HWND hwnd)
    {
        NOTIFYICONDATAW nid{ sizeof(nid) };
        nid.hWnd = hwnd;
        Shell_NotifyIconW(NIM_DELETE, &nid);
    }

    // First time the window is closed, explain that HdrSnip is still running.
    void ShowTrayHintOnce(HWND hwnd)
    {
        if (ReadFlag(L"TrayHintShown")) return;
        WriteFlag(L"TrayHintShown", true);
        NOTIFYICONDATAW nid{ sizeof(nid) };
        nid.hWnd = hwnd;
        nid.uFlags = NIF_INFO;
        nid.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
        wcscpy_s(nid.szInfoTitle, L"HdrSnip is still running");
        wcscpy_s(nid.szInfo, g_printScreen
            ? L"Press Print Screen to snip. Right-click the tray icon to quit."
            : L"Press Ctrl+Alt+S to snip. Right-click the tray icon to quit.");
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }

    // Low-level Print Screen watcher. Keyboard utilities and Snipping Tool can swallow Print Screen with
    // their own low-level hooks before Windows turns it into a hotkey; the newest hook runs first, so
    // ours sees the key before theirs. It consumes Print Screen only and passes every other key on.
    // It lives on its own thread: a hook whose thread is busy (e.g. during a capture) gets skipped
    // and eventually removed by Windows.
    DWORD g_hookThreadId = 0;
    HANDLE g_hookThread = nullptr;

    LRESULT CALLBACK PrintScreenHook(int code, WPARAM wp, LPARAM lp)
    {
        auto key = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
        if (code == HC_ACTION && key->vkCode == VK_SNAPSHOT && g_main)
        {
            if (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN) PostMessageW(g_main, kPrintScreenPressed, 0, 0);
            return 1;
        }
        return CallNextHookEx(nullptr, code, wp, lp);
    }

    HANDLE g_hookReady = nullptr;

    DWORD WINAPI PrintScreenHookThread(LPVOID)
    {
        MSG msg;
        PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE); // create the queue so WM_QUIT can reach us
        HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, PrintScreenHook, GetModuleHandleW(nullptr), 0);
        SetEvent(g_hookReady);
        if (!hook) return 1;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {}
        UnhookWindowsHookEx(hook);
        return 0;
    }

    bool StartPrintScreenHook()
    {
        g_hookReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        g_hookThread = CreateThread(nullptr, 0, PrintScreenHookThread, nullptr, 0, &g_hookThreadId);
        if (!g_hookThread) return false;
        // Wait until the hook is in place or has failed; a thread that already exited means it failed.
        HANDLE waits[] = { g_hookReady, g_hookThread };
        WaitForMultipleObjects(2, waits, FALSE, 2000);
        DWORD exitCode = 0;
        return GetExitCodeThread(g_hookThread, &exitCode) && exitCode == STILL_ACTIVE;
    }

    void StopPrintScreenHook()
    {
        if (!g_hookThread) return;
        PostThreadMessageW(g_hookThreadId, WM_QUIT, 0, 0);
        WaitForSingleObject(g_hookThread, 2000);
        CloseHandle(g_hookThread);
        CloseHandle(g_hookReady);
        g_hookThread = g_hookReady = nullptr;
    }

    void ShowLauncher()
    {
        ShowWindow(g_main, IsIconic(g_main) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(g_main);
    }

    void LayoutLauncher(HWND hwnd, BOOL repaint)
    {
        const int y = ui::Px(hwnd, 12), h = ui::Px(hwnd, 36);
        MoveWindow(GetDlgItem(hwnd, kNewSnip), ui::Px(hwnd, 12), y, ui::Px(hwnd, 92), h, repaint);
        MoveWindow(GetDlgItem(hwnd, kDelay), ui::Px(hwnd, 12 + 92 + 4), y, ui::Px(hwnd, 120), h, repaint);
        MoveWindow(GetDlgItem(hwnd, kStream), ui::Px(hwnd, 12 + 92 + 4 + 120 + 4), y, ui::Px(hwnd, 104), h, repaint);
        RECT rc;
        GetClientRect(hwnd, &rc);
        MoveWindow(GetDlgItem(hwnd, kSettings), rc.right - ui::Px(hwnd, 12 + 40), y, ui::Px(hwnd, 40), h, repaint);
    }
    int g_delaySeconds = 0;     // Snipping-Tool-style delay for New (the hotkeys are always instant)
    bool g_delayPending = false;
    bool g_showAfterDelay = false;

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

    bool LauncherVisible() { return g_main && IsWindowVisible(g_main) && !IsIconic(g_main); }

    void RestoreLauncher() { ShowWindow(g_main, OpenPreviewCount() > 0 ? SW_SHOWNOACTIVATE : SW_SHOW); }

    void SnipFromLauncher()
    {
        // Hide instantly (no fade animation) so the launcher isn't in the capture; if it was in the
        // tray, leave it there.
        const bool wasVisible = LauncherVisible();
        if (wasVisible) { ShowWindow(g_main, SW_HIDE); Sleep(80); }
        RunSnip(false);
        if (wasVisible) RestoreLauncher();
    }

    // New button (launcher or preview): waits out the delay with the launcher hidden, so you can
    // open menus or tooltips before the screen freezes.
    void NewSnipWithDelay()
    {
        if (g_delayPending) return;
        if (!g_main || g_delaySeconds == 0) { SnipFromLauncher(); return; }
        g_delayPending = true;
        g_showAfterDelay = LauncherVisible();
        ShowWindow(g_main, SW_HIDE);
        SetTimer(g_main, kDelayTimer, UINT(g_delaySeconds) * 1000, nullptr);
    }

    // --- Print Screen ownership: opt-in, switchable, and explained in the UI. ---

    // Bluetooth & devices > Keyboard > Shortcuts and hotkeys (Windows 11 26H2). Older builds had the
    // setting under Accessibility > Keyboard, which the dialog text mentions.
    constexpr wchar_t kWindowsKeyboardSettings[] = L"ms-settings:devices-keyboard";

    // "UsePrintScreen" is absent until the user has answered the first-run question.
    std::optional<bool> PrintScreenChoice()
    {
        DWORD value = 0, size = sizeof(value);
        if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"UsePrintScreen", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
            return std::nullopt;
        return value != 0;
    }

    // Windows' own "Use the Print screen key to open screen capture" (on by default in Windows 11).
    bool WindowsPrintScreenOpensSnipping()
    {
        DWORD value = 1, size = sizeof(value);
        RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Keyboard", L"PrintScreenKeyForSnippingEnabled",
            RRF_RT_REG_DWORD, nullptr, &value, &size);
        return value != 0;
    }

    void UpdateTrayTip()
    {
        NOTIFYICONDATAW nid{ sizeof(nid) };
        nid.hWnd = g_main;
        nid.uFlags = NIF_TIP | NIF_SHOWTIP;
        wcscpy_s(nid.szTip, g_printScreen ? L"HdrSnip — Print Screen to snip" : L"HdrSnip — Ctrl+Alt+S to snip");
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }

    // Takes or releases Print Screen right away; releasing hands the key straight back to Windows.
    void SetPrintScreen(bool on)
    {
        if (on && !g_printScreen)
        {
            const bool hotkey = RegisterHotKey(nullptr, kSnipPrintScreen, MOD_NOREPEAT, VK_SNAPSHOT) != FALSE;
            g_printScreen = StartPrintScreenHook() || hotkey;
        }
        else if (!on && g_printScreen)
        {
            StopPrintScreenHook();
            UnregisterHotKey(nullptr, kSnipPrintScreen);
            g_printScreen = false;
        }
        if (g_main)
        {
            UpdateTrayTip();
            InvalidateRect(g_main, nullptr, FALSE); // the hint text names the active key
        }
    }

    HRESULT CALLBACK OpenLinkCallback(HWND, UINT notification, WPARAM, LPARAM lp, LONG_PTR)
    {
        if (notification == TDN_HYPERLINK_CLICKED)
            ShellExecuteW(nullptr, L"open", reinterpret_cast<LPCWSTR>(lp), nullptr, nullptr, SW_SHOWNORMAL);
        return S_OK;
    }

    // The explanation and choice, shown on first run and from "About the Print Screen key…".
    // Returns the choice, or nothing if the dialog was dismissed.
    std::optional<bool> AskAboutPrintScreen(HWND owner)
    {
        enum { kUse = 100, kDontUse = 101 };
        const TASKDIALOG_BUTTON buttons[] = {
            { kUse, L"Yes, use HdrSnip\nPrint Screen takes an HDR-correct snip." },
            { kDontUse, L"No, keep Windows\nPrint Screen works as before. Snip with Ctrl+Alt+S." },
        };
        TASKDIALOGCONFIG cfg{ sizeof(cfg) };
        cfg.hwndParent = owner;
        cfg.hInstance = GetModuleHandleW(nullptr);
        cfg.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ENABLE_HYPERLINKS | TDF_ALLOW_DIALOG_CANCELLATION |
            TDF_POSITION_RELATIVE_TO_WINDOW;
        cfg.pszWindowTitle = L"HdrSnip";
        cfg.pszMainIcon = MAKEINTRESOURCEW(1);
        cfg.pszMainInstruction = L"Use the Print Screen key for HdrSnip?";
        cfg.pszContent =
            L"Print Screen will open HdrSnip’s snip instead of Windows Snipping Tool, while HdrSnip is "
            L"running (including from the tray). Other keys are not affected.";
        cfg.pszFooterIcon = TD_INFORMATION_ICON;
        cfg.pszFooter = L"You can change this any time under Settings (gear icon).";
        cfg.cButtons = ARRAYSIZE(buttons);
        cfg.pButtons = buttons;
        cfg.nDefaultButton = g_printScreen || !PrintScreenChoice() ? kUse : kDontUse;
        cfg.pfCallback = OpenLinkCallback;

        int pressed = 0;
        if (FAILED(TaskDialogIndirect(&cfg, &pressed, nullptr, nullptr))) return std::nullopt;
        if (pressed == kUse) return true;
        if (pressed == kDontUse) return false;
        return std::nullopt; // closed with Esc / X
    }

    // Turning it off when Windows' own Print Screen shortcut is also off would leave the key doing
    // almost nothing, so offer the Windows setting that brings Snipping Tool back.
    void OfferSnippingToolBack(HWND owner)
    {
        if (WindowsPrintScreenOpensSnipping()) return;
        enum { kOpen = 100 };
        const TASKDIALOG_BUTTON buttons[] = { { kOpen, L"Open settings" } };
        TASKDIALOGCONFIG cfg{ sizeof(cfg) };
        cfg.hwndParent = owner;
        cfg.hInstance = GetModuleHandleW(nullptr);
        cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
        cfg.dwCommonButtons = TDCBF_CLOSE_BUTTON;
        cfg.pszWindowTitle = L"HdrSnip";
        cfg.pszMainIcon = TD_INFORMATION_ICON;
        cfg.pszMainInstruction = L"Want Print Screen to open Snipping Tool again?";
        cfg.pszContent =
            L"Turn on “Use the Print screen key to open screen capture” in:\n"
            L"Settings → Bluetooth & devices → Keyboard → Shortcuts and hotkeys\n\n"
            L"On older Windows versions it is under Settings → Accessibility → Keyboard.";
        cfg.cButtons = ARRAYSIZE(buttons);
        cfg.pButtons = buttons;
        int pressed = 0;
        if (SUCCEEDED(TaskDialogIndirect(&cfg, &pressed, nullptr, nullptr)) && pressed == kOpen)
            ShellExecuteW(nullptr, L"open", kWindowsKeyboardSettings, nullptr, nullptr, SW_SHOWNORMAL);
    }

    void ApplyPrintScreenChoice(HWND owner, bool on)
    {
        WriteFlag(L"UsePrintScreen", on);
        const bool wasOn = g_printScreen;
        SetPrintScreen(on);
        if (wasOn && !on) OfferSnippingToolBack(owner);
    }

    void ExplainPrintScreen(HWND owner)
    {
        if (auto choice = AskAboutPrintScreen(owner)) ApplyPrintScreenChoice(owner, *choice);
    }

    // Settings items shared by the gear menu and the tray menu.
    enum SettingsCommand { kSetPrintScreen = 50, kSetAboutPrintScreen, kSetAutostart };

    void AppendSettingsItems(HMENU menu)
    {
        AppendMenuW(menu, MF_STRING | (g_printScreen ? MF_CHECKED : 0), kSetPrintScreen, L"Use Print Screen key");
        AppendMenuW(menu, MF_STRING, kSetAboutPrintScreen, L"About the Print Screen key…");
        AppendMenuW(menu, MF_STRING | (IsAutostartOn() ? MF_CHECKED : 0), kSetAutostart, L"Start with Windows");
    }

    bool HandleSettingsCommand(HWND owner, UINT id)
    {
        switch (id)
        {
        case kSetPrintScreen: ApplyPrintScreenChoice(owner, !g_printScreen); return true;
        case kSetAboutPrintScreen: ExplainPrintScreen(owner); return true;
        case kSetAutostart: SetAutostart(!IsAutostartOn()); return true;
        }
        return false;
    }

    void ShowSettingsMenu(HWND hwnd, HWND button)
    {
        HMENU menu = CreatePopupMenu();
        AppendSettingsItems(menu);
        RECT r;
        GetWindowRect(button, &r);
        const UINT id = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTALIGN, r.right, r.bottom, 0, hwnd, nullptr);
        DestroyMenu(menu);
        HandleSettingsCommand(hwnd, id);
    }

    void ShowTrayMenu(HWND hwnd)
    {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, kTraySnip, g_printScreen ? L"New snip\tPrint Screen" : L"New snip\tCtrl+Alt+S");
        AppendMenuW(menu, MF_STRING, kTrayStream, L"Stream…");
        AppendMenuW(menu, MF_STRING, kTrayOpen, L"Open HdrSnip");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendSettingsItems(menu);
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kTrayQuit, L"Quit");
        SetMenuDefaultItem(menu, kTrayOpen, FALSE);

        POINT pt;
        GetCursorPos(&pt);
        SetForegroundWindow(hwnd); // required, or the menu won't close when clicking elsewhere
        const UINT id = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
        DestroyMenu(menu);
        if (HandleSettingsCommand(hwnd, id)) return;
        switch (id)
        {
        case kTraySnip: SnipFromLauncher(); break;
        case kTrayStream: ChooseStreamSource(hwnd); break;
        case kTrayOpen: ShowLauncher(); break;
        case kTrayQuit: DestroyWindow(hwnd); break;
        }
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
            g_printScreen ? L"Select New or press Print Screen to start a snip."
                          : L"Select New or press Ctrl+Alt+S to start a snip.",
            body, t.text, DT_CENTER | DT_SINGLELINE | DT_BOTTOM);
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
        if (msg == g_taskbarCreated && g_taskbarCreated)
        {
            AddTrayIcon(hwnd);
            return 0;
        }
        switch (msg)
        {
        case kPrintScreenPressed:
            SnipFromLauncher();
            return 0;
        case kTrayMessage: // NOTIFYICON_VERSION_4: the event is in LOWORD(lp)
            switch (LOWORD(lp))
            {
            case NIN_SELECT:
            case NIN_KEYSELECT:
                ShowLauncher();
                break;
            case WM_CONTEXTMENU:
                ShowTrayMenu(hwnd);
                break;
            }
            return 0;
        case WM_CLOSE:
            ShowWindow(hwnd, SW_HIDE);
            ShowTrayHintOnce(hwnd);
            return 0;
        case WM_CREATE:
        {
            ui::CreateButton(hwnd, kNewSnip, ui::kGlyphAdd, L"New", ui::ButtonStyle::Accent);
            ui::CreateButton(hwnd, kDelay, ui::kGlyphDelay, DelayLabel(g_delaySeconds).c_str(), ui::ButtonStyle::Subtle);
            ui::CreateButton(hwnd, kStream, ui::kGlyphStream, L"Stream", ui::ButtonStyle::Subtle);
            ui::CreateButton(hwnd, kSettings, ui::kGlyphSettings, L"", ui::ButtonStyle::Subtle);
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
            if (LOWORD(wp) == kSettings) ShowSettingsMenu(hwnd, GetDlgItem(hwnd, kSettings));
            return 0;
        case WM_TIMER:
            if (wp == kDelayTimer)
            {
                KillTimer(hwnd, kDelayTimer);
                g_delayPending = false;
                RunSnip(false);
                if (g_showAfterDelay) RestoreLauncher();
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
            RemoveTrayIcon(hwnd);
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void CreateLauncher(bool show)
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
        g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        AddTrayIcon(g_main);
        if (show) ShowWindow(g_main, SW_SHOW);
    }

    void RunMessageLoop(bool exitWhenNoPreviews)
    {
        MSG msg;
        while ((!exitWhenNoPreviews || OpenPreviewCount() > 0) && GetMessageW(&msg, nullptr, 0, 0) > 0)
        {
            if (msg.message == WM_HOTKEY && (msg.wParam == kSnip || msg.wParam == kSnipPrintScreen)) SnipFromLauncher();
            else if (msg.message == WM_HOTKEY && msg.wParam == kQuit)
            {
                if (!g_main) break;
                DestroyWindow(g_main); // removes the tray icon, then posts WM_QUIT
            }
            if (g_main && msg.message == WM_KEYDOWN && msg.wParam == 'N' && GetKeyState(VK_CONTROL) < 0 &&
                GetAncestor(msg.hwnd, GA_ROOT) == g_main)
            {
                NewSnipWithDelay();
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
        if (arg != L"--tray") // works when it's hidden in the tray, too
        {
            ShowWindow(existing, IsIconic(existing) ? SW_RESTORE : SW_SHOW);
            SetForegroundWindow(existing);
        }
        return 0;
    }

    // Hotkeys are a bonus; the launcher still works if another app owns them. Print Screen is opt-in:
    // it goes through the low-level watcher (reliable even when other apps hook the key), with a
    // registered hotkey as a fallback in case the watcher can't start. A key consumed by the watcher
    // never reaches the hotkey, so one press snips once.
    RegisterHotKey(nullptr, kSnip, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'S');
    RegisterHotKey(nullptr, kQuit, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'Q');
    const auto printScreenChoice = PrintScreenChoice();
    SetPrintScreen(printScreenChoice.value_or(false)); // before the tray tooltip/hint read it
    CreateLauncher(arg != L"--tray"); // --tray: start hidden (used by Start with Windows)
    // First run: explain Print Screen and let the user choose (not when starting silently in the tray).
    if (!printScreenChoice && arg != L"--tray") ExplainPrintScreen(g_main);
    if (arg == L"--stream") OpenShareWindow(arg2.c_str()); // optional: part of a window title
    RunMessageLoop(false);
    SetPrintScreen(false);
    UnregisterHotKey(nullptr, kSnip);
    UnregisterHotKey(nullptr, kQuit);
    UnregisterHotKey(nullptr, kSnipPrintScreen);
    return 0;
}
