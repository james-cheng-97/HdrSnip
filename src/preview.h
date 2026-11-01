#pragma once
#include <windows.h>
#include <cstdint>
#include <vector>

// A finished snip, cropped: SDR for display/clipboard/PNG, scRGB floats for the HDR master.
struct SnipImage
{
    int width = 0;
    int height = 0;
    float sdrWhiteNits = 80.0f;
    std::vector<uint8_t> sdrBgra;
    std::vector<float> hdrRgba;
};

// Opens a Snipping-Tool-style result window (Save as / Copy). Non-modal; owns the image.
void ShowPreview(HMONITOR monitor, SnipImage image);

// What the preview's "New" button runs (the preview closes itself first).
void SetNewSnipHandler(void (*handler)());

// Number of preview windows still open.
int OpenPreviewCount();

// Routes Ctrl+S / Ctrl+C to the preview that has focus. Returns true if handled.
bool HandlePreviewKey(const MSG& msg);
