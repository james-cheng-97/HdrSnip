#pragma once
#include <cstdint>
#include <filesystem>

// All functions take a sub-rectangle of a larger image via its row stride (in pixels).

// SDR copy for pasting anywhere (Discord, chat, docs). The clipboard has no HDR format.
bool CopyBgraToClipboard(const uint8_t* bgra, int width, int height, int stridePixels);

void SavePng(const std::filesystem::path& path, const uint8_t* bgra, int width, int height, int stridePixels);

// Full HDR master: JPEG XR holding scRGB half-floats, the same format Xbox Game Bar uses.
void SaveJxr(const std::filesystem::path& path, const float* rgba, int width, int height, int stridePixels);

// iPhone-style HDR: an SDR JPEG (bgra) with an embedded gain map derived from the scRGB master (rgba).
// HDR-capable viewers brighten the highlights; everything else just shows the SDR image.
void SaveUltraHdrJpeg(const std::filesystem::path& path, const uint8_t* bgra, const float* rgba,
                      int width, int height, int stridePixels, float sdrWhiteNits);

// %USERPROFILE%\Pictures\HdrSnip\snip_YYYYMMDD_HHMMSS (no extension)
std::filesystem::path NextOutputBasePath();
