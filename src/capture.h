#pragma once
#include <windows.h>
#include <vector>

// One captured monitor frame in scRGB: linear, Rec.709 primaries, 1.0 = 80 nits.
// Values may exceed 1.0 (HDR highlights) or go negative (colors outside sRGB gamut).
struct HdrFrame
{
    int width = 0;
    int height = 0;
    std::vector<float> rgba; // width * height * 4, tightly packed
};

// Grabs a single frame of the monitor via Windows.Graphics.Capture in FP16.
HdrFrame CaptureMonitor(HMONITOR monitor);
