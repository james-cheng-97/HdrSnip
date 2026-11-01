#pragma once
#include <windows.h>
#include <cstdint>
#include <optional>

// Shows the frozen SDR frame full-screen on the monitor and lets the user drag a region.
// Returns the region in image pixels; a click without dragging selects the whole monitor.
// Esc or right-click cancels.
std::optional<RECT> SelectRegion(HMONITOR monitor, const uint8_t* bgra, int width, int height);
