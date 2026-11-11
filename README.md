# HdrSnip

A small HDR-correct screen snipping tool for Windows (with a Discord share window planned).
Built from scratch rather than forked from OBS, so it isn't bound by OBS's GPL.

## Why
On an HDR desktop Windows composes in scRGB (FP16, values above 1.0). Most capture tools grab
8-bit and get washed-out or blown-out results. HdrSnip captures FP16 via Windows.Graphics.Capture,
reads the monitor's **SDR content brightness**, and tone-maps properly:
- SDR content (UI, text) keeps its exact colors.
- Brighter-than-SDR highlights roll off smoothly instead of clipping.

## Use
Launch `HdrSnip.exe` to open a small window with a **+ New snip** button (Ctrl+N). Closing the window
hides HdrSnip to the notification area (tray), where it keeps listening for **Print Screen** and
**Ctrl+Alt+S**. Click the tray icon to reopen the window; right-click it for New snip, Stream,
**Start with Windows** and Quit. Launching the exe again brings the existing window forward.

**Print Screen:** Windows 11 gives this key to its own Snipping Tool by default. Turn off
*Settings → Accessibility → Keyboard → "Use the Print screen key to open screen capture"*, then restart
HdrSnip. The window's hint text says which key is active. If another app (keyboard software, OneDrive,
Dropbox) still holds Print Screen, HdrSnip falls back to Ctrl+Alt+S.

| | |
|---|---|
| `+ New snip` / `Ctrl+N` / `Print Screen` / `Ctrl+Alt+S` | Snip: drag a region (a click alone = whole monitor; Esc/right-click cancels) |
| Delay button | No delay / 3 / 5 / 10 seconds before **New** freezes the screen (remembered; the hotkey is always instant) |
| `Ctrl+Alt+Q` | Quit |
| `HdrSnip.exe --tray` | Start hidden in the tray (what Start with Windows uses) |
| `HdrSnip.exe --snip` | One-shot snip; exits when the preview closes |
| `HdrSnip.exe --fullscreen` | Headless: capture the monitor under the cursor and save PNG + JXR to `Pictures\HdrSnip` |

Like Snipping Tool, a snip is **not saved automatically**. It is copied to the clipboard (SDR, ready
to paste into Discord etc.) and opened in a preview window:
- **Save as… (Ctrl+S)**, three formats:
  - PNG: SDR, lossless, opens everywhere
  - JPEG Ultra HDR: iPhone-style. An SDR JPEG with an embedded gain map, so HDR viewers
    (Chrome/Edge on an HDR display, recent Android/iOS/macOS) show real highlights and everything else shows SDR
  - JPEG XR: full HDR master (scRGB half-float, like Xbox Game Bar)
- **Copy (Ctrl+C)**: copy again
- **Esc**: close

## Streaming to Discord
**Stream** in the launcher shows a menu of your displays and open windows. Pick one and HdrSnip opens a
stream window: a live, GPU tone-mapped copy of it, drawn as plain SDR. In Discord pick **Go Live →
Applications →** that window, and viewers get correct colors instead of the washed-out HDR capture.
- **Window source** (recommended for games): only that game is streamed, even when you alt-tab. The stream
  window takes the game's title (and follows it), so Discord's picker and label show the game's name.
  If the game closes, the stream window closes too (ending the Discord stream); it never switches to
  another source on its own. Games must run in borderless/windowed mode;
  exclusive fullscreen can't be captured per window.
- **Display source**: everything visible on that display.
- Change the source any time: **Stream** again, or right-click the stream window's title bar → *Change source…*
- It opens on another display when you have one. On a single display, keep it *behind* your game
  (not minimized): Discord still captures it.
- The window's size is the stream resolution; resize it to change quality.
- Command line: `HdrSnip.exe --stream ["part of a window title"]` (works while HdrSnip is already running;
  if no window matches, nothing is streamed)

## Download
Get `HdrSnip-<version>-win-x64.zip` from Releases, unzip, run `HdrSnip.exe`. No installer.
The exe isn't code-signed, so Windows SmartScreen may warn the first time (More info → Run anyway).

## Build
Needs Visual Studio with the C++ desktop workload and Windows SDK 10.0.22621+.
```
cmake -S . -B build
cmake --build build --config Release
```

To publish a release: push a tag such as `v1.0.0`. The `build` workflow compiles it on GitHub and
attaches the zip to a new release.

## License
MIT, see [LICENSE](LICENSE).

## Layout
- `src/capture.*`: WGC single-frame FP16 capture
- `src/tonemap.*`: SDR white level lookup and scRGB → sRGB tone mapping
- `src/overlay.*`: full-screen region selector
- `src/preview.*`: result window with Save as / Copy
- `src/share.*`: Discord stream window (live capture, HLSL tone map, SDR swap chain)
- `src/ui.*`: Windows 11 theme, title bar and buttons
- `src/output.*`: clipboard, PNG, JXR and Ultra HDR JPEG writers (gain map + XMP + MPF container, no extra libraries)
- `src/main.cpp`: hotkeys and command-line modes

## Roadmap
1. Ultra HDR: also write the ISO 21496-1 metadata block (newer viewers prefer it over the XMP form).
2. Settings: custom hotkey, default save folder and format.
3. Stream overlay (webcam corner, text) and streaming a single window or region instead of a whole display.
4. Optional virtual-camera output.
