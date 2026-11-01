#include "capture.h"

#include <d3d11.h>
#include <dxgi.h>
#include <DirectXPackedVector.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <algorithm>
#include <stdexcept>

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgd = winrt::Windows::Graphics::DirectX;

HdrFrame CaptureMonitor(HMONITOR monitor)
{
    winrt::com_ptr<ID3D11Device> d3d;
    winrt::com_ptr<ID3D11DeviceContext> ctx;
    winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, d3d.put(), nullptr, ctx.put()));

    winrt::com_ptr<::IInspectable> inspectable;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(d3d.as<IDXGIDevice>().get(), inspectable.put()));
    auto device = inspectable.as<wgd::Direct3D11::IDirect3DDevice>();

    auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    wgc::GraphicsCaptureItem item{ nullptr };
    winrt::check_hresult(interop->CreateForMonitor(monitor, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item)));

    // FP16 keeps the desktop in scRGB; an 8-bit format would make Windows clip HDR content for us.
    auto pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
        device, wgd::DirectXPixelFormat::R16G16B16A16Float, 1, item.Size());
    auto session = pool.CreateCaptureSession(item);
    session.IsCursorCaptureEnabled(false);
    try { session.IsBorderRequired(false); } catch (...) {} // older Windows builds lack this
    session.StartCapture();

    wgc::Direct3D11CaptureFrame frame{ nullptr };
    for (int i = 0; i < 400 && !frame; ++i)
    {
        frame = pool.TryGetNextFrame();
        if (!frame) Sleep(5);
    }
    session.Close();
    pool.Close();
    if (!frame) throw std::runtime_error("No frame received from Windows.Graphics.Capture");

    auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    winrt::com_ptr<ID3D11Texture2D> texture;
    winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(texture.put())));

    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    winrt::com_ptr<ID3D11Texture2D> staging;
    winrt::check_hresult(d3d->CreateTexture2D(&desc, nullptr, staging.put()));
    ctx->CopyResource(staging.get(), texture.get());

    auto content = frame.ContentSize();
    HdrFrame out;
    out.width = std::min<int>(content.Width, desc.Width);
    out.height = std::min<int>(content.Height, desc.Height);
    out.rgba.resize(size_t(out.width) * out.height * 4);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    winrt::check_hresult(ctx->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
    for (int y = 0; y < out.height; ++y)
    {
        auto src = reinterpret_cast<const DirectX::PackedVector::HALF*>(
            static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch);
        DirectX::PackedVector::XMConvertHalfToFloatStream(
            &out.rgba[size_t(y) * out.width * 4], sizeof(float), src, sizeof(DirectX::PackedVector::HALF),
            size_t(out.width) * 4);
    }
    ctx->Unmap(staging.get(), 0);
    return out;
}
