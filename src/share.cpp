#include "share.h"
#include "tonemap.h"
#include "ui.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <DispatcherQueue.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.System.h>

#include <algorithm>
#include <dwmapi.h>
#include <optional>
#include <string>
#include <vector>

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgd = winrt::Windows::Graphics::DirectX;

namespace
{
    constexpr wchar_t kClass[] = L"HdrSnipShare";
    constexpr UINT kMenuChangeSource = 0x1000; // system-menu IDs: below 0xF000, low 4 bits reserved
    constexpr UINT_PTR kTitleTimer = 1;
    constexpr auto kFormat = wgd::DirectXPixelFormat::R16G16B16A16Float;

    // Same curve as the snip tone mapper (tonemap.cpp), but with a fixed peak taken from the display,
    // since a per-frame peak would make brightness pump while streaming. The sRGB render target view
    // does the gamma encode.
    constexpr char kShader[] = R"(
Texture2D src : register(t0);
SamplerState smp : register(s0);
cbuffer Params : register(b0) { float scale; float knee; float tMax; float compress; };
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vs(uint id : SV_VertexID)
{
    VSOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 ps(VSOut i) : SV_Target
{
    float3 c = max(src.Sample(smp, i.uv).rgb * scale, 0);
    float m = max(c.r, max(c.g, c.b));
    if (compress > 0 && m > knee)
    {
        float t = (m - knee) / (1 - knee);
        float mapped = knee + (1 - knee) * t * (1 + t / (tMax * tMax)) / (1 + t);
        c *= mapped / m;
    }
    return float4(min(c, 1), 1);
}
)";

    struct Params { float scale, knee, tMax, compress; };

    struct Share
    {
        HWND hwnd = nullptr;
        HWND sourceWindow = nullptr; // null when streaming a whole display
        std::wstring sourceTitle;

        winrt::com_ptr<ID3D11Device> d3d;
        winrt::com_ptr<ID3D11DeviceContext> ctx;
        winrt::com_ptr<IDXGISwapChain1> swap;
        winrt::com_ptr<ID3D11VertexShader> vs;
        winrt::com_ptr<ID3D11PixelShader> ps;
        winrt::com_ptr<ID3D11SamplerState> sampler;
        winrt::com_ptr<ID3D11Buffer> params;
        winrt::com_ptr<ID3D11Texture2D> frame;      // our copy of the latest capture
        winrt::com_ptr<ID3D11ShaderResourceView> frameView;

        wgd::Direct3D11::IDirect3DDevice device{ nullptr };
        wgc::Direct3D11CaptureFramePool pool{ nullptr };
        wgc::GraphicsCaptureSession session{ nullptr };
        wgc::Direct3D11CaptureFramePool::FrameArrived_revoker frameArrived;
        winrt::Windows::Graphics::SizeInt32 poolSize{};
    };

    Share* g_share = nullptr;

    float PeakNits(HMONITOR monitor)
    {
        winrt::com_ptr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.put())))) return 1000.0f;
        winrt::com_ptr<IDXGIAdapter1> adapter;
        for (UINT a = 0; factory->EnumAdapters1(a, adapter.put()) == S_OK; ++a, adapter = nullptr)
        {
            winrt::com_ptr<IDXGIOutput> output;
            for (UINT o = 0; adapter->EnumOutputs(o, output.put()) == S_OK; ++o, output = nullptr)
            {
                DXGI_OUTPUT_DESC desc{};
                output->GetDesc(&desc);
                DXGI_OUTPUT_DESC1 desc1{};
                if (desc.Monitor == monitor && output.try_as<IDXGIOutput6>() &&
                    SUCCEEDED(output.as<IDXGIOutput6>()->GetDesc1(&desc1)))
                    return desc1.MaxLuminance;
            }
        }
        return 1000.0f;
    }

    struct Source
    {
        HMONITOR monitor = nullptr; // set for a display
        HWND window = nullptr;      // set for a single window
        std::wstring label;
    };

    std::wstring WindowTitle(HWND hwnd)
    {
        wchar_t title[256] = {};
        GetWindowTextW(hwnd, title, ARRAYSIZE(title));
        return title;
    }

    std::vector<Source> ListDisplays()
    {
        std::vector<HMONITOR> monitors;
        EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM data) {
            reinterpret_cast<std::vector<HMONITOR>*>(data)->push_back(m);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&monitors));

        std::vector<Source> out;
        for (size_t i = 0; i < monitors.size(); ++i)
        {
            MONITORINFO mi{ sizeof(mi) };
            GetMonitorInfoW(monitors[i], &mi);
            out.push_back({ monitors[i], nullptr, L"Display " + std::to_wstring(i + 1) + L" (" +
                std::to_wstring(mi.rcMonitor.right - mi.rcMonitor.left) + L"×" +
                std::to_wstring(mi.rcMonitor.bottom - mi.rcMonitor.top) + L")" +
                ((mi.dwFlags & MONITORINFOF_PRIMARY) ? L", main" : L"") });
        }
        return out;
    }

    // Top-level app windows a person would recognize: visible, titled, not tool windows, not ours.
    std::vector<Source> ListWindows()
    {
        std::vector<Source> out;
        EnumWindows([](HWND hwnd, LPARAM data) {
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            BOOL cloaked = FALSE;
            DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
            if (!IsWindowVisible(hwnd) || cloaked || pid == GetCurrentProcessId() || GetWindow(hwnd, GW_OWNER) ||
                (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW))
                return TRUE;
            std::wstring title = WindowTitle(hwnd);
            if (!title.empty() && title != L"Program Manager")
                reinterpret_cast<std::vector<Source>*>(data)->push_back({ nullptr, hwnd, title });
            return TRUE;
        }, reinterpret_cast<LPARAM>(&out));
        return out;
    }

    void OpenStream(const Source& source); // opens the stream window, or switches it to this source

    Source MainDisplay()
    {
        for (auto& d : ListDisplays())
        {
            MONITORINFO mi{ sizeof(mi) };
            GetMonitorInfoW(d.monitor, &mi);
            if (mi.dwFlags & MONITORINFOF_PRIMARY) return d;
        }
        return ListDisplays().front();
    }

    void Render(Share& s)
    {
        if (!s.swap) return;
        winrt::com_ptr<ID3D11Texture2D> back;
        if (FAILED(s.swap->GetBuffer(0, IID_PPV_ARGS(back.put())))) return;
        D3D11_TEXTURE2D_DESC bd{};
        back->GetDesc(&bd);
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        winrt::com_ptr<ID3D11RenderTargetView> rtv;
        winrt::check_hresult(s.d3d->CreateRenderTargetView(back.get(), &rd, rtv.put()));

        const float black[4] = { 0, 0, 0, 1 };
        s.ctx->ClearRenderTargetView(rtv.get(), black);
        if (s.frameView)
        {
            // Letterbox to keep the source aspect ratio.
            D3D11_TEXTURE2D_DESC fd{};
            s.frame->GetDesc(&fd);
            const float scale = std::min(float(bd.Width) / fd.Width, float(bd.Height) / fd.Height);
            const float w = fd.Width * scale, h = fd.Height * scale;
            D3D11_VIEWPORT vp{ (bd.Width - w) / 2, (bd.Height - h) / 2, w, h, 0, 1 };

            ID3D11RenderTargetView* targets[] = { rtv.get() };
            ID3D11ShaderResourceView* views[] = { s.frameView.get() };
            ID3D11SamplerState* samplers[] = { s.sampler.get() };
            ID3D11Buffer* buffers[] = { s.params.get() };
            s.ctx->OMSetRenderTargets(1, targets, nullptr);
            s.ctx->RSSetViewports(1, &vp);
            s.ctx->IASetInputLayout(nullptr);
            s.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            s.ctx->VSSetShader(s.vs.get(), nullptr, 0);
            s.ctx->PSSetShader(s.ps.get(), nullptr, 0);
            s.ctx->PSSetShaderResources(0, 1, views);
            s.ctx->PSSetSamplers(0, 1, samplers);
            s.ctx->PSSetConstantBuffers(0, 1, buffers);
            s.ctx->Draw(3, 0);
            ID3D11RenderTargetView* none[] = { nullptr };
            s.ctx->OMSetRenderTargets(1, none, nullptr);
        }
        s.swap->Present(1, 0);
    }

    void OnFrame(Share& s)
    {
        auto frame = s.pool.TryGetNextFrame();
        if (!frame) return;

        auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        winrt::com_ptr<ID3D11Texture2D> texture;
        winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(texture.put())));
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);

        D3D11_TEXTURE2D_DESC have{};
        if (s.frame) s.frame->GetDesc(&have);
        if (!s.frame || have.Width != desc.Width || have.Height != desc.Height)
        {
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            desc.CPUAccessFlags = 0;
            desc.MiscFlags = 0;
            s.frame = nullptr;
            s.frameView = nullptr;
            winrt::check_hresult(s.d3d->CreateTexture2D(&desc, nullptr, s.frame.put()));
            winrt::check_hresult(s.d3d->CreateShaderResourceView(s.frame.get(), nullptr, s.frameView.put()));
        }
        s.ctx->CopyResource(s.frame.get(), texture.get());

        // Display resolution changed: resize the pool so the next frames are the full size.
        const auto size = frame.ContentSize();
        if (size.Width != s.poolSize.Width || size.Height != s.poolSize.Height)
        {
            s.poolSize = size;
            s.pool.Recreate(s.device, kFormat, 2, size);
        }
        frame.Close();
        Render(s);
    }

    // Window title = what Discord shows in its picker and stream label, so mirror the source's name.
    void UpdateTitle(Share& s, const std::wstring& title)
    {
        if (title == s.sourceTitle) return;
        s.sourceTitle = title;
        SetWindowTextW(s.hwnd, title.c_str());
    }

    // Keep the stream window's width and match the source's aspect ratio, so Discord gets no black bars.
    void FitToSource(Share& s, winrt::Windows::Graphics::SizeInt32 size)
    {
        if (size.Width <= 0 || size.Height <= 0 || IsZoomed(s.hwnd)) return;
        RECT rc;
        GetClientRect(s.hwnd, &rc);
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(MonitorFromWindow(s.hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        const int maxH = (mi.rcWork.bottom - mi.rcWork.top) * 9 / 10;
        int cw = std::max<LONG>(rc.right, 320), ch = MulDiv(cw, size.Height, size.Width);
        if (ch > maxH) { ch = maxH; cw = MulDiv(ch, size.Width, size.Height); }
        RECT r{ 0, 0, cw, ch };
        AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, GetDpiForWindow(s.hwnd));
        SetWindowPos(s.hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void StopCapture(Share& s)
    {
        s.frameArrived.revoke();
        if (s.session) { s.session.Close(); s.session = nullptr; }
        if (s.pool) { s.pool.Close(); s.pool = nullptr; }
        s.frame = nullptr;
        s.frameView = nullptr;
        s.sourceWindow = nullptr;
    }

    // A newly picked source failed: show black rather than keep streaming the previous one, so nothing
    // reaches the stream that the user didn't just choose.
    void PauseCapture(Share& s)
    {
        StopCapture(s);
        UpdateTitle(s, L"HdrSnip Stream — paused (pick a source)");
        Render(s);
    }

    void StartCapture(Share& s, const Source& source)
    {
        StopCapture(s);
        s.sourceWindow = source.window;
        const HMONITOR monitor = source.window ? MonitorFromWindow(source.window, MONITOR_DEFAULTTONEAREST) : source.monitor;

        const float sdrWhite = GetSdrWhiteNits(monitor);
        const float peak = std::max(PeakNits(monitor) / sdrWhite, 1.0f);
        const Params p{ 80.0f / sdrWhite, 0.9f, (peak - 0.9f) / 0.1f, peak > 1.0f ? 1.0f : 0.0f };
        s.ctx->UpdateSubresource(s.params.get(), 0, nullptr, &p, 0, 0);

        auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        wgc::GraphicsCaptureItem item{ nullptr };
        if (source.window)
            winrt::check_hresult(interop->CreateForWindow(source.window, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item)));
        else
            winrt::check_hresult(interop->CreateForMonitor(monitor, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(item)));

        // Non-free-threaded pool: FrameArrived is delivered on this (UI) thread via its DispatcherQueue.
        s.poolSize = item.Size();
        s.pool = wgc::Direct3D11CaptureFramePool::Create(s.device, kFormat, 2, s.poolSize);
        s.frameArrived = s.pool.FrameArrived(winrt::auto_revoke, [](auto&&, auto&&) { if (g_share) OnFrame(*g_share); });
        s.session = s.pool.CreateCaptureSession(item);
        s.session.IsCursorCaptureEnabled(true);
        try { s.session.IsBorderRequired(false); } catch (...) {}
        s.session.StartCapture();

        UpdateTitle(s, source.window ? WindowTitle(source.window) : L"HdrSnip Stream — " + source.label);
        FitToSource(s, s.poolSize);
    }

    std::optional<Source> PickSource(HWND owner)
    {
        auto displays = ListDisplays(), windows = ListWindows();
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"Displays");
        for (size_t i = 0; i < displays.size(); ++i)
            AppendMenuW(menu, MF_STRING, 1 + i, (L"    " + displays[i].label).c_str());
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"Windows (use borderless mode for games)");
        for (size_t i = 0; i < windows.size(); ++i)
        {
            std::wstring label = windows[i].label.size() > 60 ? windows[i].label.substr(0, 57) + L"…" : windows[i].label;
            UINT flags = MF_STRING | (g_share && g_share->sourceWindow == windows[i].window ? MF_CHECKED : 0);
            AppendMenuW(menu, flags, 1 + displays.size() + i, (L"    " + label).c_str());
        }

        POINT pt;
        GetCursorPos(&pt);
        SetForegroundWindow(owner);
        const UINT id = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, 0, owner, nullptr);
        DestroyMenu(menu);
        if (id == 0) return std::nullopt;
        return id <= displays.size() ? displays[id - 1] : windows[id - 1 - displays.size()];
    }

    void InitGraphics(Share& s)
    {
        winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, s.d3d.put(), nullptr, s.ctx.put()));
        auto dxgiDevice = s.d3d.as<IDXGIDevice>();
        winrt::com_ptr<::IInspectable> inspectable;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectable.put()));
        s.device = inspectable.as<wgd::Direct3D11::IDirect3DDevice>();

        winrt::com_ptr<IDXGIAdapter> adapter;
        winrt::check_hresult(dxgiDevice->GetAdapter(adapter.put()));
        winrt::com_ptr<IDXGIFactory2> factory;
        winrt::check_hresult(adapter->GetParent(IID_PPV_ARGS(factory.put())));
        RECT rc;
        GetClientRect(s.hwnd, &rc);
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = UINT(std::max<LONG>(rc.right, 1));
        sd.Height = UINT(std::max<LONG>(rc.bottom, 1));
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; // SDR on purpose: this is what Discord should see
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.Scaling = DXGI_SCALING_STRETCH;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        winrt::check_hresult(factory->CreateSwapChainForHwnd(s.d3d.get(), s.hwnd, &sd, nullptr, nullptr, s.swap.put()));

        auto compile = [](const char* entry, const char* target) {
            winrt::com_ptr<ID3DBlob> code, errors;
            winrt::check_hresult(D3DCompile(kShader, sizeof(kShader) - 1, nullptr, nullptr, nullptr, entry, target,
                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.put(), errors.put()));
            return code;
        };
        auto vsCode = compile("vs", "vs_5_0"), psCode = compile("ps", "ps_5_0");
        winrt::check_hresult(s.d3d->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, s.vs.put()));
        winrt::check_hresult(s.d3d->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, s.ps.put()));

        D3D11_SAMPLER_DESC samp{};
        samp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU = samp.AddressV = samp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samp.MaxLOD = D3D11_FLOAT32_MAX;
        winrt::check_hresult(s.d3d->CreateSamplerState(&samp, s.sampler.put()));

        D3D11_BUFFER_DESC cb{ sizeof(Params), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER };
        winrt::check_hresult(s.d3d->CreateBuffer(&cb, nullptr, s.params.put()));
    }

    LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        Share* s = g_share && g_share->hwnd == hwnd ? g_share : nullptr;
        switch (msg)
        {
        case WM_SIZE:
            if (s && s->swap && wp != SIZE_MINIMIZED)
            {
                s->ctx->ClearState();
                s->swap->ResizeBuffers(0, std::max(1, int(LOWORD(lp))), std::max(1, int(HIWORD(lp))), DXGI_FORMAT_UNKNOWN, 0);
                Render(*s);
            }
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            ValidateRect(hwnd, nullptr);
            if (s) Render(*s);
            return 0;
        case WM_SYSCOMMAND:
            if (s && (wp & 0xFFF0) == kMenuChangeSource)
            {
                ChooseStreamSource(hwnd);
                return 0;
            }
            break;
        case WM_TIMER:
            // Follow the captured window: rename with it, and close the stream (never switch to another
            // source) when it closes, which also ends the Discord stream.
            if (s && s->sourceWindow)
            {
                if (!IsWindow(s->sourceWindow)) DestroyWindow(hwnd);
                else UpdateTitle(*s, WindowTitle(s->sourceWindow));
            }
            return 0;
        case WM_SETTINGCHANGE:
            if (lp && wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0)
            {
                ui::Refresh();
                ui::ApplyChrome(hwnd);
            }
            return 0;
        case WM_DESTROY:
            if (s)
            {
                s->frameArrived.revoke();
                if (s->session) s->session.Close();
                if (s->pool) s->pool.Close();
                delete s;
                g_share = nullptr;
            }
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void EnsureDispatcherQueue()
    {
        static winrt::Windows::System::DispatcherQueueController controller{ nullptr };
        if (controller || winrt::Windows::System::DispatcherQueue::GetForCurrentThread()) return;
        DispatcherQueueOptions options{ sizeof(options), DQTYPE_THREAD_CURRENT, DQTAT_COM_NONE };
        winrt::check_hresult(CreateDispatcherQueueController(options,
            reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(winrt::put_abi(controller))));
    }
}

void OpenShareWindow(const wchar_t* windowTitleContains)
{
    if (!windowTitleContains || !*windowTitleContains) { OpenStream(MainDisplay()); return; }
    for (auto& w : ListWindows())
        if (w.label.find(windowTitleContains) != std::wstring::npos) { OpenStream(w); return; }
    // Asked for a specific window: never substitute the whole display.
    MessageBoxW(nullptr, (std::wstring(L"No open window matches \"") + windowTitleContains + L"\".").c_str(),
        L"HdrSnip", MB_ICONINFORMATION);
}

void ChooseStreamSource(HWND owner)
{
    if (auto source = PickSource(owner)) OpenStream(*source);
}

namespace {
void OpenStream(const Source& source)
{
    if (g_share)
    {
        try
        {
            StartCapture(*g_share, source);
        }
        catch (const winrt::hresult_error& e)
        {
            PauseCapture(*g_share); // don't keep streaming the previous source by surprise
            MessageBoxW(g_share->hwnd, (L"Can't capture that source: " + std::wstring(e.message())).c_str(),
                L"HdrSnip", MB_ICONERROR);
        }
        ShowWindow(g_share->hwnd, SW_SHOWNOACTIVATE);
        return;
    }

    static bool registered = [] {
        WNDCLASSW wc{};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
        wc.lpszClassName = kClass;
        return RegisterClassW(&wc) != 0;
    }();
    if (!registered) return;

    try
    {
        EnsureDispatcherQueue();
        auto s = std::make_unique<Share>();
        const HMONITOR sourceMonitor = source.window ? MonitorFromWindow(source.window, MONITOR_DEFAULTTONEAREST) : source.monitor;

        // Open on another display if there is one, so the window doesn't cover what it streams.
        HMONITOR home = sourceMonitor;
        for (auto& d : ListDisplays()) if (d.monitor != sourceMonitor) { home = d.monitor; break; }
        MONITORINFO dst{ sizeof(dst) };
        GetMonitorInfoW(home, &dst);
        const int workW = dst.rcWork.right - dst.rcWork.left, workH = dst.rcWork.bottom - dst.rcWork.top;
        RECT r{ 0, 0, std::min(1920, workW * 7 / 10), std::min(1080, workH * 7 / 10) }; // FitToSource fixes the aspect
        AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);
        const int w = r.right - r.left, h = r.bottom - r.top;

        g_share = s.release();
        g_share->hwnd = CreateWindowExW(0, kClass, L"HdrSnip Stream", WS_OVERLAPPEDWINDOW,
            dst.rcWork.left + (workW - w) / 2, dst.rcWork.top + (workH - h) / 2, w, h,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        ui::ApplyChrome(g_share->hwnd);

        HMENU menu = GetSystemMenu(g_share->hwnd, FALSE);
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kMenuChangeSource, L"Change source…");
        SetTimer(g_share->hwnd, kTitleTimer, 1000, nullptr);

        InitGraphics(*g_share);
        StartCapture(*g_share, source);
        ShowWindow(g_share->hwnd, SW_SHOW);
    }
    catch (const winrt::hresult_error& e)
    {
        if (g_share && g_share->hwnd) DestroyWindow(g_share->hwnd); // WM_DESTROY frees it
        else { delete g_share; g_share = nullptr; }
        MessageBoxW(nullptr, (L"Couldn't start the stream window: " + std::wstring(e.message())).c_str(),
            L"HdrSnip", MB_ICONERROR);
    }
}
}
