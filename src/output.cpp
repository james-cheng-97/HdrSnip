#include "output.h"

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <DirectXPackedVector.h>
#include <winrt/base.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

bool CopyBgraToClipboard(const uint8_t* bgra, int width, int height, int stridePixels)
{
    const size_t imageBytes = size_t(width) * height * 4;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + imageBytes);
    if (!mem) return false;

    auto header = static_cast<BITMAPINFOHEADER*>(GlobalLock(mem));
    *header = { sizeof(BITMAPINFOHEADER), width, height, 1, 32, BI_RGB }; // CF_DIB is bottom-up
    auto dst = reinterpret_cast<uint8_t*>(header + 1);
    for (int y = 0; y < height; ++y)
        std::memcpy(dst + size_t(height - 1 - y) * width * 4, bgra + size_t(y) * stridePixels * 4, size_t(width) * 4);
    GlobalUnlock(mem);

    if (!OpenClipboard(nullptr)) { GlobalFree(mem); return false; }
    EmptyClipboard();
    bool ok = SetClipboardData(CF_DIB, mem) != nullptr;
    CloseClipboard();
    if (!ok) GlobalFree(mem);
    return ok;
}

namespace
{
    void EncodeTo(IStream* stream, const GUID& container, WICPixelFormatGUID format,
                  const void* pixels, int width, int height, UINT strideBytes, float jpegQuality = 0)
    {
        auto factory = winrt::create_instance<IWICImagingFactory>(CLSID_WICImagingFactory);

        winrt::com_ptr<IWICBitmapEncoder> encoder;
        winrt::check_hresult(factory->CreateEncoder(container, nullptr, encoder.put()));
        winrt::check_hresult(encoder->Initialize(stream, WICBitmapEncoderNoCache));

        winrt::com_ptr<IWICBitmapFrameEncode> frame;
        winrt::com_ptr<IPropertyBag2> options;
        winrt::check_hresult(encoder->CreateNewFrame(frame.put(), options.put()));
        if (jpegQuality > 0)
        {
            PROPBAG2 name{};
            name.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
            VARIANT value{};
            value.vt = VT_R4;
            value.fltVal = jpegQuality;
            winrt::check_hresult(options->Write(1, &name, &value));
        }
        winrt::check_hresult(frame->Initialize(options.get()));
        winrt::check_hresult(frame->SetSize(width, height));
        WICPixelFormatGUID requested = format;
        winrt::check_hresult(frame->SetPixelFormat(&format));
        if (format != requested) throw winrt::hresult_error(WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT);
        winrt::check_hresult(frame->WritePixels(height, strideBytes, strideBytes * height,
            static_cast<BYTE*>(const_cast<void*>(pixels))));
        winrt::check_hresult(frame->Commit());
        winrt::check_hresult(encoder->Commit());
    }

    void Encode(const std::filesystem::path& path, const GUID& container, WICPixelFormatGUID format,
                const void* pixels, int width, int height, UINT strideBytes)
    {
        winrt::com_ptr<IStream> stream;
        winrt::check_hresult(SHCreateStreamOnFileEx(path.c_str(), STGM_CREATE | STGM_WRITE, FILE_ATTRIBUTE_NORMAL,
            TRUE, nullptr, stream.put()));
        EncodeTo(stream.get(), container, format, pixels, width, height, strideBytes);
    }

    std::vector<uint8_t> EncodeJpeg(WICPixelFormatGUID format, const void* pixels, int width, int height,
                                    UINT strideBytes, float quality)
    {
        winrt::com_ptr<IStream> stream;
        winrt::check_hresult(CreateStreamOnHGlobal(nullptr, TRUE, stream.put()));
        EncodeTo(stream.get(), GUID_ContainerFormatJpeg, format, pixels, width, height, strideBytes, quality);

        HGLOBAL mem = nullptr;
        winrt::check_hresult(GetHGlobalFromStream(stream.get(), &mem));
        STATSTG stat{};
        winrt::check_hresult(stream->Stat(&stat, STATFLAG_NONAME));
        auto data = static_cast<const uint8_t*>(GlobalLock(mem));
        std::vector<uint8_t> bytes(data, data + stat.cbSize.QuadPart);
        GlobalUnlock(mem);
        return bytes;
    }

    // --- Ultra HDR container helpers (Android Ultra HDR v1 / Adobe hdrgm XMP + CIPA Multi-Picture Format) ---

    void PutBE16(std::vector<uint8_t>& v, uint32_t x) { v.push_back(uint8_t(x >> 8)); v.push_back(uint8_t(x)); }
    void PutBE32(std::vector<uint8_t>& v, uint32_t x) { PutBE16(v, x >> 16); PutBE16(v, x & 0xFFFF); }

    std::vector<uint8_t> AppSegment(uint8_t marker, const std::string& payload)
    {
        std::vector<uint8_t> seg{ 0xFF, marker };
        PutBE16(seg, uint32_t(payload.size() + 2));
        seg.insert(seg.end(), payload.begin(), payload.end());
        return seg;
    }

    std::vector<uint8_t> XmpSegment(const std::string& description)
    {
        std::string xmp = std::string("http://ns.adobe.com/xap/1.0/") + '\0' +
            "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\" x:xmptk=\"HdrSnip\">"
            "<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">" +
            description + "</rdf:RDF></x:xmpmeta>";
        return AppSegment(0xE1, xmp);
    }

    // MPF APP2 listing two images. The gain map offset is relative to the TIFF header ("MM"),
    // which sits 8 bytes into the segment (FF E2, length, "MPF\0").
    std::vector<uint8_t> MpfSegment(uint32_t primarySize, uint32_t gainMapSize, uint32_t gainMapOffset)
    {
        std::vector<uint8_t> p{ 'M', 'P', 'F', 0, 'M', 'M', 0, 0x2A };
        PutBE32(p, 8);                                // first IFD right after the TIFF header
        PutBE16(p, 3);                                // entry count
        PutBE16(p, 0xB000); PutBE16(p, 7); PutBE32(p, 4); p.insert(p.end(), { '0', '1', '0', '0' }); // version
        PutBE16(p, 0xB001); PutBE16(p, 4); PutBE32(p, 1); PutBE32(p, 2);                              // image count
        PutBE16(p, 0xB002); PutBE16(p, 7); PutBE32(p, 32); PutBE32(p, 8 + 2 + 3 * 12 + 4);           // MP entries
        PutBE32(p, 0);                                // no next IFD
        PutBE32(p, 0x030000); PutBE32(p, primarySize); PutBE32(p, 0); PutBE32(p, 0);                // primary, baseline
        PutBE32(p, 0); PutBE32(p, gainMapSize); PutBE32(p, gainMapOffset); PutBE32(p, 0);
        return AppSegment(0xE2, std::string(p.begin(), p.end()));
    }

    std::string Num(float v)
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.6f", v);
        return buf;
    }
}

void SaveUltraHdrJpeg(const std::filesystem::path& path, const uint8_t* bgra, const float* rgba,
                      int width, int height, int stridePixels, float sdrWhiteNits)
{
    // 1. Gain map: log2 of HDR/SDR luminance per pixel, box-downscaled 4x (gain maps are smooth,
    //    and viewers upsample them). The SDR base is the tone-mapped image the user already sees.
    constexpr int kScale = 4;
    constexpr float kOffset = 1.0f / 64.0f; // keeps blacks from dividing by zero
    const float hdrScale = 80.0f / sdrWhiteNits;
    float srgbToLinear[256];
    for (int i = 0; i < 256; ++i)
    {
        float s = i / 255.0f;
        srgbToLinear[i] = s <= 0.04045f ? s / 12.92f : std::pow((s + 0.055f) / 1.055f, 2.4f);
    }
    auto luma = [](float r, float g, float b) { return 0.2126f * r + 0.7152f * g + 0.0722f * b; };

    const int gw = (width + kScale - 1) / kScale, gh = (height + kScale - 1) / kScale;
    std::vector<float> logGain(size_t(gw) * gh);
    float maxGain = 0.0f;
    for (int gy = 0; gy < gh; ++gy)
        for (int gx = 0; gx < gw; ++gx)
        {
            float sum = 0;
            int n = 0;
            for (int y = gy * kScale; y < std::min(height, (gy + 1) * kScale); ++y)
                for (int x = gx * kScale; x < std::min(width, (gx + 1) * kScale); ++x)
                {
                    const uint8_t* s = bgra + (size_t(y) * stridePixels + x) * 4;
                    const float* h = rgba + (size_t(y) * stridePixels + x) * 4;
                    float ys = luma(srgbToLinear[s[2]], srgbToLinear[s[1]], srgbToLinear[s[0]]);
                    float yh = luma(std::max(h[0], 0.0f), std::max(h[1], 0.0f), std::max(h[2], 0.0f)) * hdrScale;
                    sum += std::max(0.0f, std::log2((yh + kOffset) / (ys + kOffset))); // SDR never brighter than HDR
                    ++n;
                }
            float g = sum / n;
            logGain[size_t(gy) * gw + gx] = g;
            maxGain = std::max(maxGain, g);
        }
    maxGain = std::max(maxGain, 0.01f); // spec needs max > min even for pure-SDR snips

    std::vector<uint8_t> gainPixels(logGain.size());
    for (size_t i = 0; i < logGain.size(); ++i)
        gainPixels[i] = uint8_t(std::lround(std::clamp(logGain[i] / maxGain, 0.0f, 1.0f) * 255.0f));

    // 2. Encode both images as plain JPEGs.
    std::vector<uint8_t> bgr(size_t(width) * height * 3);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            std::memcpy(&bgr[(size_t(y) * width + x) * 3], bgra + (size_t(y) * stridePixels + x) * 4, 3);
    auto primary = EncodeJpeg(GUID_WICPixelFormat24bppBGR, bgr.data(), width, height, UINT(width) * 3, 0.95f);
    auto gainJpeg = EncodeJpeg(GUID_WICPixelFormat8bppGray, gainPixels.data(), gw, gh, UINT(gw), 0.90f);

    // 3. Gain map JPEG gets the hdrgm parameters.
    const std::string g = Num(maxGain), off = Num(kOffset);
    auto gainXmp = XmpSegment(
        "<rdf:Description xmlns:hdrgm=\"http://ns.adobe.com/hdr-gain-map/1.0/\" hdrgm:Version=\"1.0\""
        " hdrgm:GainMapMin=\"0\" hdrgm:GainMapMax=\"" + g + "\" hdrgm:Gamma=\"1\""
        " hdrgm:OffsetSDR=\"" + off + "\" hdrgm:OffsetHDR=\"" + off + "\""
        " hdrgm:HDRCapacityMin=\"0\" hdrgm:HDRCapacityMax=\"" + g + "\" hdrgm:BaseRenditionIsHDR=\"False\"/>");
    std::vector<uint8_t> gainMap{ 0xFF, 0xD8 };
    gainMap.insert(gainMap.end(), gainXmp.begin(), gainXmp.end());
    gainMap.insert(gainMap.end(), gainJpeg.begin() + 2, gainJpeg.end());

    // 4. Primary JPEG gets the container XMP and the MPF index pointing at the appended gain map.
    auto primaryXmp = XmpSegment(
        "<rdf:Description xmlns:Container=\"http://ns.google.com/photos/1.0/container/\""
        " xmlns:Item=\"http://ns.google.com/photos/1.0/container/item/\""
        " xmlns:hdrgm=\"http://ns.adobe.com/hdr-gain-map/1.0/\" hdrgm:Version=\"1.0\">"
        "<Container:Directory><rdf:Seq>"
        "<rdf:li rdf:parseType=\"Resource\"><Container:Item Item:Semantic=\"Primary\" Item:Mime=\"image/jpeg\"/></rdf:li>"
        "<rdf:li rdf:parseType=\"Resource\"><Container:Item Item:Semantic=\"GainMap\" Item:Mime=\"image/jpeg\""
        " Item:Length=\"" + std::to_string(gainMap.size()) + "\"/></rdf:li>"
        "</rdf:Seq></Container:Directory></rdf:Description>");

    const size_t mpfSize = MpfSegment(0, 0, 0).size();
    const uint32_t primarySize = uint32_t(2 + primaryXmp.size() + mpfSize + primary.size() - 2);
    const uint32_t tiffHeaderPos = uint32_t(2 + primaryXmp.size() + 8);
    auto mpf = MpfSegment(primarySize, uint32_t(gainMap.size()), primarySize - tiffHeaderPos);

    std::vector<uint8_t> file{ 0xFF, 0xD8 };
    file.insert(file.end(), primaryXmp.begin(), primaryXmp.end());
    file.insert(file.end(), mpf.begin(), mpf.end());
    file.insert(file.end(), primary.begin() + 2, primary.end());
    file.insert(file.end(), gainMap.begin(), gainMap.end());

    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(file.data()), std::streamsize(file.size()));
    if (!out) throw std::runtime_error("Could not write file");
}

void SavePng(const std::filesystem::path& path, const uint8_t* bgra, int width, int height, int stridePixels)
{
    // Alpha is always 255, so write BGR and skip a pointless alpha channel in the file.
    std::vector<uint8_t> bgr(size_t(width) * height * 3);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            std::memcpy(&bgr[(size_t(y) * width + x) * 3], bgra + (size_t(y) * stridePixels + x) * 4, 3);
    Encode(path, GUID_ContainerFormatPng, GUID_WICPixelFormat24bppBGR, bgr.data(), width, height, UINT(width) * 3);
}

void SaveJxr(const std::filesystem::path& path, const float* rgba, int width, int height, int stridePixels)
{
    using DirectX::PackedVector::HALF;
    std::vector<HALF> halves(size_t(width) * height * 4);
    for (int y = 0; y < height; ++y)
        DirectX::PackedVector::XMConvertFloatToHalfStream(&halves[size_t(y) * width * 4], sizeof(HALF),
            rgba + size_t(y) * stridePixels * 4, sizeof(float), size_t(width) * 4);
    for (size_t i = 3; i < halves.size(); i += 4) halves[i] = 0x3C00; // alpha = 1.0
    Encode(path, GUID_ContainerFormatWmp, GUID_WICPixelFormat64bppRGBAHalf, halves.data(), width, height,
        UINT(width) * 4 * sizeof(HALF));
}

std::filesystem::path NextOutputBasePath()
{
    PWSTR pictures = nullptr;
    winrt::check_hresult(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &pictures));
    std::filesystem::path dir = std::filesystem::path(pictures) / L"HdrSnip";
    CoTaskMemFree(pictures);
    std::filesystem::create_directories(dir);

    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[64];
    swprintf_s(name, L"snip_%04d%02d%02d_%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return dir / name;
}
