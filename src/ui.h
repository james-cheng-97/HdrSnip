#pragma once
#include <windows.h>

// Windows 11 (Fluent) look for plain Win32: follows the system light/dark setting.
namespace ui
{
    struct Theme
    {
        bool dark;
        COLORREF bg, canvas, text, subtext, hover, pressed, border;
        COLORREF accent, accentHover, accentPressed, accentText;
    };

    void Init();                 // once at startup (GDI+, button class, theme)
    const Theme& Current();
    void Refresh();              // re-read light/dark; call on WM_SETTINGCHANGE
    void ApplyChrome(HWND hwnd); // dark/light title bar that blends into the window

    int Px(HWND hwnd, int dip);
    HFONT TextFont(HWND hwnd, int pt = 10, int weight = FW_NORMAL); // caller deletes
    HFONT IconFont(HWND hwnd, int pt = 11);                          // caller deletes

    // Fills r with color and draws single-line text in it.
    void DrawLabel(HDC dc, RECT r, const wchar_t* text, HFONT font, COLORREF color, UINT flags);

    // Custom button: rounded, hover/press states, sends WM_COMMAND(id) to the parent on click.
    // Accent = filled blue (primary action); Subtle = transparent until hovered.
    enum class ButtonStyle { Accent, Subtle };
    HWND CreateButton(HWND parent, int id, const wchar_t* glyph, const wchar_t* text, ButtonStyle style);
    void SetButtonText(HWND button, const wchar_t* text);

    // Segoe Fluent Icons code points used by the app.
    inline constexpr wchar_t kGlyphAdd[] = L"";
    inline constexpr wchar_t kGlyphSave[] = L"";
    inline constexpr wchar_t kGlyphCopy[] = L"";
    inline constexpr wchar_t kGlyphStream[] = L"";
    inline constexpr wchar_t kGlyphDelay[] = L"";
}
