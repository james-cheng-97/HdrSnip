#include "tonemap.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cwchar>

float GetSdrWhiteNits(HMONITOR monitor)
{
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return 80.0f;

    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) return 80.0f;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS)
        return 80.0f;

    for (UINT32 i = 0; i < pathCount; ++i)
    {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = paths[i].sourceInfo.adapterId;
        source.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) continue;
        if (wcscmp(source.viewGdiDeviceName, info.szDevice) != 0) continue;

        DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
        white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        white.header.size = sizeof(white);
        white.header.adapterId = paths[i].targetInfo.adapterId;
        white.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS)
            return white.SDRWhiteLevel / 1000.0f * 80.0f; // documented scale: 1000 == 80 nits
    }
    return 80.0f;
}

namespace
{
    constexpr int kLutSize = 4096;

    const std::array<uint8_t, kLutSize + 1>& SrgbLut()
    {
        static const auto lut = [] {
            std::array<uint8_t, kLutSize + 1> t{};
            for (int i = 0; i <= kLutSize; ++i)
            {
                float v = float(i) / kLutSize;
                float s = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
                t[i] = uint8_t(std::lround(std::clamp(s, 0.0f, 1.0f) * 255.0f));
            }
            return t;
        }();
        return lut;
    }
}

std::vector<uint8_t> ToneMapToBgra8(const HdrFrame& frame, float sdrWhiteNits)
{
    const size_t count = size_t(frame.width) * frame.height;
    const float scale = 80.0f / sdrWhiteNits; // after this, 1.0 == SDR white
    const float* px = frame.rgba.data();

    float peak = 1.0f;
    for (size_t i = 0; i < count; ++i)
        peak = std::max(peak, std::max({ px[i * 4], px[i * 4 + 1], px[i * 4 + 2] }) * scale);

    // Values below the knee pass through untouched; [knee, peak] is squeezed into [knee, 1]
    // with an extended-Reinhard curve on max(R,G,B), which keeps hue and hits 1.0 exactly at peak.
    constexpr float knee = 0.9f;
    const float tMax = (peak - knee) / (1.0f - knee);
    const bool compress = peak > 1.0f;

    const auto& lut = SrgbLut();
    std::vector<uint8_t> out(count * 4);
    for (size_t i = 0; i < count; ++i)
    {
        float r = std::max(px[i * 4] * scale, 0.0f); // negative = out of sRGB gamut; clip
        float g = std::max(px[i * 4 + 1] * scale, 0.0f);
        float b = std::max(px[i * 4 + 2] * scale, 0.0f);

        float m = std::max({ r, g, b });
        if (compress && m > knee)
        {
            float t = (m - knee) / (1.0f - knee);
            float mapped = knee + (1.0f - knee) * t * (1.0f + t / (tMax * tMax)) / (1.0f + t);
            float k = mapped / m;
            r *= k; g *= k; b *= k;
        }

        auto enc = [&](float v) { return lut[size_t(std::min(v, 1.0f) * kLutSize + 0.5f)]; };
        out[i * 4] = enc(b);
        out[i * 4 + 1] = enc(g);
        out[i * 4 + 2] = enc(r);
        out[i * 4 + 3] = 255;
    }
    return out;
}
