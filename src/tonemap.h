#pragma once
#include "capture.h"
#include <cstdint>
#include <vector>

// The "SDR content brightness" slider for this monitor, in nits (80 when HDR is off).
float GetSdrWhiteNits(HMONITOR monitor);

// scRGB -> 8-bit sRGB, BGRA order, alpha 255. SDR content keeps its exact colors;
// only values brighter than SDR white are rolled off so highlights don't clip flat.
std::vector<uint8_t> ToneMapToBgra8(const HdrFrame& frame, float sdrWhiteNits);
