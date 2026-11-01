#pragma once
#include <windows.h>

// The stream window: a live, HDR-corrected mirror of a display or a single window, rendered as plain
// SDR so Discord's Go Live capture gets correct colors. When streaming a window, the stream window
// takes that window's title, so Discord labels the stream with the game's name.

// With text: streams the first window whose title contains it (message if none; never a display).
// Without: streams the main display.
void OpenShareWindow(const wchar_t* windowTitleContains = nullptr);

// Pops up a menu of displays and windows at the cursor; the pick opens or retargets the stream window.
void ChooseStreamSource(HWND owner);
