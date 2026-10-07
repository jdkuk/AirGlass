#include "ui/glass_window.h"

#include <d3d11_4.h>
#include <dwmapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include "ps_compose.h"
#include "ps_convert.h"
#include "vs_full.h"

namespace {

constexpr UINT kMsgSettled = WM_APP + 201;
constexpr UINT kMsgDismissed = WM_APP + 202;
constexpr UINT_PTR kTimerIdle = 7;
constexpr UINT_PTR kTimerDebugFsIn = 8, kTimerDebugFsOut = 9;
constexpr UINT_PTR kTimerTvPress = 10;
constexpr float kRadiusDp = 30.0f;  // superellipse radius (reads like ~22 dip circular)
constexpr float kBezelDp = 7.0f;
constexpr float kShadowOffsetDp = 8.0f, kShadowSoftDp = 11.0f, kShadowAlpha = 0.36f;
constexpr float kControlsIdleSeconds = 2.2f;

struct ComposeCB {
    float view[4], slab[4], video[4], geom[4], appear[4], shadow[4], vinfo[4], pill[4], pillInfo[4], drop[4], btnX[4],
        btnState[4], icons[4], label[4], labelInfo[4], tv0[4], tv1[4];
};
struct ConvertCB {
    float row0[4], row1[4], row2[4], info[4];
};

void Set4(float* d, float a, float b, float c, float e) {
    d[0] = a;
    d[1] = b;
    d[2] = c;
    d[3] = e;
}

void ColorMatrix(const VideoColor& c, ConvertCB& cb) {
    double kr = 0.2126, kb = 0.0722;
    if (c.matrix == 601) { kr = 0.299; kb = 0.114; }
    if (c.matrix == 2020) { kr = 0.2627; kb = 0.0593; }
    double kg = 1.0 - kr - kb;
    double ys = c.fullRange ? 1.0 : 255.0 / 219.0, yo = c.fullRange ? 0.0 : 16.0 / 255.0;
    double cs = c.fullRange ? 1.0 : 255.0 / 224.0, co = 128.0 / 255.0;
    double rCr = 2.0 * (1.0 - kr) * cs;
    double bCb = 2.0 * (1.0 - kb) * cs;
    double gCb = -2.0 * kb * (1.0 - kb) / kg * cs;
    double gCr = -2.0 * kr * (1.0 - kr) / kg * cs;
    Set4(cb.row0, float(ys), 0.0f, float(rCr), float(-ys * yo - rCr * co));
    Set4(cb.row1, float(ys), float(gCb), float(gCr), float(-ys * yo - gCb * co - gCr * co));
    Set4(cb.row2, float(ys), float(bCb), 0.0f, float(-ys * yo - bCb * co));
}

void StepSpring(float& x, float& v, float target, float k, float zeta, float dt) {
    float c = 2.0f * zeta * std::sqrt(k);
    int n = std::max(1, int(std::ceil(dt / 0.004f)));
    float h = dt / float(n);
    for (int i = 0; i < n; ++i) {
        float a = -k * (x - target) - c * v;
        v += a * h;
        x += v * h;
    }
}

bool Near(float x, float v, float target, float eps) { return std::fabs(x - target) < eps && std::fabs(v) < eps * 30.0f; }

float GuessAspect(const std::string& model) {
    if (model.find("iPad") != std::string::npos) return 0.75f;
    if (model.find("Mac") != std::string::npos) return 1.6f;
    if (model.find("AppleTV") != std::string::npos || model.find("Vision") != std::string::npos) return 16.0f / 9.0f;
    return 1179.0f / 2556.0f;  // iPhone portrait
}

RectF FitAspect(const RectF& box, float aspect) {
    float w = box.W(), h = box.W() / aspect;
    if (h > box.H()) {
        h = box.H();
        w = h * aspect;
    }
    return {box.CX() - w * 0.5f, box.CY() - h * 0.5f, box.CX() + w * 0.5f, box.CY() + h * 0.5f};
}

bool SameRect(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

}  // namespace

GlassWindow::GlassWindow() = default;
GlassWindow::~GlassWindow() { Destroy(); }

// ---------------------------------------------------------------------------------------------
// Setup

bool GlassWindow::CreateDevice() {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.GetAddressOf())))) return false;
    // Use the adapter driving the primary display.
    ComPtr<IDXGIAdapter1> chosen;
    HMONITOR primary = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++i) {
        ComPtr<IDXGIOutput> out;
        for (UINT j = 0; adapter->EnumOutputs(j, out.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++j) {
            DXGI_OUTPUT_DESC od;
            if (SUCCEEDED(out->GetDesc(&od)) && od.Monitor == primary && !chosen) chosen = adapter;
        }
    }
    if (chosen) {
        DXGI_ADAPTER_DESC1 ad;
        chosen->GetDesc1(&ad);
        LOGI("gpu: %s", WideToUtf8(ad.Description).c_str());
    }
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    D3D_FEATURE_LEVEL got;
    auto create = [&](UINT f) {
        return D3D11CreateDevice(chosen.Get(), chosen ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, f,
                                 levels, 2, D3D11_SDK_VERSION, dev_.ReleaseAndGetAddressOf(), &got,
                                 ctx_.ReleaseAndGetAddressOf());
    };
    HRESULT hr = create(flags);
    if (FAILED(hr)) {
        LOGW("gpu: device without video support (0x%08lx)", hr);
        hr = create(D3D11_CREATE_DEVICE_BGRA_SUPPORT);
    }
    if (FAILED(hr)) {
        LOGE("gpu: D3D11CreateDevice failed 0x%08lx", hr);
        return false;
    }
    ComPtr<ID3D11Multithread> mt;
    if (SUCCEEDED(ctx_.As(mt))) mt->SetMultithreadProtected(TRUE);
    return true;
}

bool GlassWindow::CreateSwapChain() {
    // Starts small; EnsureSwapSize() matches the window before the first frame.
    bufW_ = 64;
    bufH_ = 64;

    ComPtr<IDXGIDevice> dxgiDev;
    dev_.As(dxgiDev);
    ComPtr<IDXGIAdapter> adapter;
    dxgiDev->GetAdapter(adapter.GetAddressOf());
    ComPtr<IDXGIFactory2> factory;
    adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(factory.GetAddressOf()));

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = bufW_;
    sd.Height = bufH_;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    ComPtr<IDXGISwapChain1> sc1;
    HRESULT hr = factory->CreateSwapChainForComposition(dev_.Get(), &sd, nullptr, sc1.GetAddressOf());
    if (FAILED(hr)) {
        LOGE("gpu: CreateSwapChainForComposition failed 0x%08lx", hr);
        return false;
    }
    sc1.As(swap_);
    swap_->SetMaximumFrameLatency(1);
    frameWait_ = swap_->GetFrameLatencyWaitableObject();

    ComPtr<ID3D11Texture2D> back;
    swap_->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(back.GetAddressOf()));
    dev_->CreateRenderTargetView(back.Get(), nullptr, rtv_.GetAddressOf());

    hr = DCompositionCreateDevice(dxgiDev.Get(), __uuidof(IDCompositionDevice),
                                  reinterpret_cast<void**>(dcomp_.GetAddressOf()));
    if (FAILED(hr)) {
        LOGE("gpu: DCompositionCreateDevice failed 0x%08lx", hr);
        return false;
    }
    dcomp_->CreateTargetForHwnd(hwnd_, TRUE, dtarget_.GetAddressOf());
    dcomp_->CreateVisual(dvisual_.GetAddressOf());
    dvisual_->SetContent(swap_.Get());
    dtarget_->SetRoot(dvisual_.Get());
    dcomp_->Commit();
    LOGI("gpu: composition swap chain %ux%u", bufW_, bufH_);
    return true;
}

bool GlassWindow::CreatePipeline() {
    if (FAILED(dev_->CreateVertexShader(g_VSFull, sizeof(g_VSFull), nullptr, vsFull_.GetAddressOf())) ||
        FAILED(dev_->CreatePixelShader(g_PSCompose, sizeof(g_PSCompose), nullptr, psCompose_.GetAddressOf())) ||
        FAILED(dev_->CreatePixelShader(g_PSConvert, sizeof(g_PSConvert), nullptr, psConvert_.GetAddressOf()))) {
        LOGE("gpu: shader creation failed");
        return false;
    }
    auto makeCb = [&](UINT size, ComPtr<ID3D11Buffer>& out) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = (size + 15) & ~15u;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        return SUCCEEDED(dev_->CreateBuffer(&bd, nullptr, out.GetAddressOf()));
    };
    if (!makeCb(sizeof(ComposeCB), cbCompose_) || !makeCb(sizeof(ConvertCB), cbConvert_)) return false;
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    if (FAILED(dev_->CreateSamplerState(&sd, sampLinear_.GetAddressOf()))) return false;

    D2D1_FACTORY_OPTIONS fo{};
    D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory1), &fo,
                      reinterpret_cast<void**>(d2d_.GetAddressOf()));
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()));
    return true;
}

bool GlassWindow::Create(HINSTANCE inst, HICON bigIcon, HICON smallIcon, const Options& opt) {
    opt_ = opt;
    pinned_ = opt.pinned;
    char env[64];
    if (GetEnvironmentVariableA("AIRGLASS_DEBUG_CONTROLS", env, sizeof(env))) debugHover_ = atoi(env);
    if (GetEnvironmentVariableA("AIRGLASS_DEBUG_FULLSCREEN", env, sizeof(env)))
        sscanf(env, "%f,%f", &debugFsIn_, &debugFsOut_);
    wchar_t wenv[512];
    if (GetEnvironmentVariableW(L"AIRGLASS_DEBUG_SNAPSHOT", wenv, 512)) {
        snapDir_ = wenv;
        char times[512] = "";
        GetEnvironmentVariableA("AIRGLASS_DEBUG_SNAPSHOT_TIMES", times, sizeof(times));
        for (char* tok = strtok(times, ","); tok; tok = strtok(nullptr, ",")) snapTimes_.push_back(float(atof(tok)));
    }
    if (!CreateDevice()) return false;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = StaticWndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = bigIcon;
    wc.hIconSm = smallIcon;
    wc.lpszClassName = L"AirGlassWindow";
    RegisterClassExW(&wc);
    hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_APPWINDOW, L"AirGlassWindow", L"AirGlass",
                            WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_SYSMENU, 100, 100, 400, 800, nullptr,
                            nullptr, inst, this);
    if (!hwnd_) {
        LOGE("ui: CreateWindow failed %lu", GetLastError());
        return false;
    }
    dp_ = float(GetDpiForWindow(hwnd_)) / 96.0f;
    GetWindowRect(hwnd_, &winRect_);

    DWMNCRENDERINGPOLICY ncr = DWMNCRP_DISABLED;
    DwmSetWindowAttribute(hwnd_, DWMWA_NCRENDERING_POLICY, &ncr, sizeof(ncr));
    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    COLORREF border = DWMWA_COLOR_NONE;
    DwmSetWindowAttribute(hwnd_, DWMWA_BORDER_COLOR, &border, sizeof(border));
    BOOL noTransitions = TRUE;
    DwmSetWindowAttribute(hwnd_, DWMWA_TRANSITIONS_FORCEDISABLED, &noTransitions, sizeof(noTransitions));

    if (!CreateSwapChain() || !CreatePipeline()) return false;
    {
        std::lock_guard<std::mutex> lk(stateMu_);
        shared_.dp = dp_;
        shared_.pinned = pinned_;
    }
    quit_ = false;
    renderThread_ = std::thread(&GlassWindow::RenderThread, this);
    return true;
}

void GlassWindow::Destroy() {
    if (renderThread_.joinable()) {
        quit_ = true;
        wake_.Set();
        renderThread_.join();
    }
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    std::lock_guard<std::recursive_mutex> g(gpuLock_);
    dvisual_.Reset();
    dtarget_.Reset();
    dcomp_.Reset();
    rtv_.Reset();
    swap_.Reset();
}

// ---------------------------------------------------------------------------------------------
// Frame sink (decoder threads)

void GlassWindow::PresentHwFrame(ID3D11Texture2D* tex, unsigned index, int w, int h, const VideoColor& c) {
    std::lock_guard<std::recursive_mutex> g(gpuLock_);
    if (!dev_) return;
    if (!nv12_ || nv12W_ != w || nv12H_ != h) {
        D3D11_TEXTURE2D_DESC src;
        tex->GetDesc(&src);
        if (src.Format != DXGI_FORMAT_NV12) {
            LOGE("gpu: decoder surface format %d is not NV12", int(src.Format));
            return;
        }
        nv12_.Reset();
        nv12Y_.Reset();
        nv12UV_.Reset();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = UINT(w);
        td.Height = UINT(h);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_NV12;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, nv12_.GetAddressOf()))) {
            LOGE("gpu: cannot create NV12 texture %dx%d", w, h);
            return;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        sd.Format = DXGI_FORMAT_R8_UNORM;
        dev_->CreateShaderResourceView(nv12_.Get(), &sd, nv12Y_.GetAddressOf());
        sd.Format = DXGI_FORMAT_R8G8_UNORM;
        dev_->CreateShaderResourceView(nv12_.Get(), &sd, nv12UV_.GetAddressOf());
        nv12W_ = w;
        nv12H_ = h;
    }
    D3D11_BOX box{0, 0, 0, UINT(w), UINT(h), 1};
    ctx_->CopySubresourceRegion(nv12_.Get(), 0, 0, 0, 0, tex, index, &box);
    srcW_ = w;
    srcH_ = h;
    srcKind_ = 1;
    srcColor_ = c;
    frameSeq_.fetch_add(1);
    Wake();
}

void GlassWindow::PresentSwFrame(const uint8_t* const planes[3], const int strides[3], bool nv12, int w, int h,
                                 const VideoColor& c) {
    std::lock_guard<std::recursive_mutex> g(gpuLock_);
    if (!dev_) return;
    auto ensure = [&](ComPtr<ID3D11Texture2D>& t, ComPtr<ID3D11ShaderResourceView>& s, int tw, int th, DXGI_FORMAT f) {
        D3D11_TEXTURE2D_DESC d{};
        if (t) t->GetDesc(&d);
        if (t && int(d.Width) == tw && int(d.Height) == th && d.Format == f) return;
        t.Reset();
        s.Reset();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = UINT(tw);
        td.Height = UINT(th);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = f;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        dev_->CreateTexture2D(&td, nullptr, t.GetAddressOf());
        if (t) dev_->CreateShaderResourceView(t.Get(), nullptr, s.GetAddressOf());
    };
    int cw = (w + 1) / 2, ch = (h + 1) / 2;
    ensure(swY_, swYSrv_, w, h, DXGI_FORMAT_R8_UNORM);
    if (!swY_) return;
    ctx_->UpdateSubresource(swY_.Get(), 0, nullptr, planes[0], UINT(strides[0]), 0);
    if (nv12) {
        ensure(swUV_, swUVSrv_, cw, ch, DXGI_FORMAT_R8G8_UNORM);
        if (!swUV_) return;
        ctx_->UpdateSubresource(swUV_.Get(), 0, nullptr, planes[1], UINT(strides[1]), 0);
    } else {
        ensure(swU_, swUSrv_, cw, ch, DXGI_FORMAT_R8_UNORM);
        ensure(swV_, swVSrv_, cw, ch, DXGI_FORMAT_R8_UNORM);
        if (!swU_ || !swV_) return;
        ctx_->UpdateSubresource(swU_.Get(), 0, nullptr, planes[1], UINT(strides[1]), 0);
        ctx_->UpdateSubresource(swV_.Get(), 0, nullptr, planes[2], UINT(strides[2]), 0);
    }
    srcW_ = w;
    srcH_ = h;
    srcKind_ = nv12 ? 3 : 2;
    srcColor_ = c;
    frameSeq_.fetch_add(1);
    Wake();
}

void GlassWindow::Wake() {
    needFrame_ = true;
    wake_.Set();
}

// ---------------------------------------------------------------------------------------------
// Rendering

GlassWindow::Layout GlassWindow::ComputeLayout(const RectF& slab, float fs, float dp, float aspect, int buttons,
                                               float boost) {
    Layout L;
    L.slab = slab;
    L.buttons = buttons;
    float minSide = std::max(1.0f, std::min(slab.W(), slab.H()));
    float maxSide = std::max(slab.W(), slab.H());
    // Corner radius and glass rim grow with the window so the glass reads at any size.
    float radius = std::clamp(0.060f * minSide, kRadiusDp * dp * 0.8f, kRadiusDp * dp * 2.7f);
    L.radius = std::min(radius, minSide * 0.5f) * (1.0f - fs);
    L.bezel = std::clamp(0.0085f * maxSide, kBezelDp * dp, kBezelDp * dp * 2.5f) * (1.0f - fs);
    RectF inner = slab.Inflate(-L.bezel);
    L.video = FitAspect(inner, std::max(aspect, 0.05f));
    L.uiScale = std::clamp(std::min(L.video.W(), L.video.H()) / (300.0f * dp), 0.72f, 1.3f);
    if (boost > 1.0f) {
        // Bigger for the couch, but the capsule must stay inside the picture.
        float fit = 0.85f * L.video.W() / (dp * (40.0f * float(buttons) + 8.0f));
        L.uiScale = std::max(L.uiScale, std::min(L.uiScale * boost, fit));
    }
    float s = dp * L.uiScale;
    float pillH = 40.0f * s, pad = 4.0f * s;
    L.spacing = 40.0f * s;
    float pillW = L.spacing * float(buttons) + pad * 2.0f;
    float top = L.video.y0 + (14.0f + 12.0f * fs) * s;
    L.pill = {L.video.CX() - pillW * 0.5f, top, L.video.CX() + pillW * 0.5f, top + pillH};
    for (int i = 0; i < 4; ++i) L.btnX[i] = i < buttons ? L.pill.x0 + pad + L.spacing * (float(i) + 0.5f) : -1e4f;
    L.btnY = L.pill.CY();
    L.btnR = 16.0f * s;
    L.iconHalf = 8.0f * s;
    L.stroke = 2.1f * s;
    return L;
}

void GlassWindow::RenderThread() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    while (!quit_) {
        if (!needFrame_.exchange(false)) {
            wake_.Wait(100);
            continue;
        }
        {
            std::lock_guard<std::mutex> lk(stateMu_);
            if (!shared_.visible || shared_.iconic) continue;
        }
        if (frameWait_) WaitForSingleObjectEx(frameWait_, 50, FALSE);
        std::lock_guard<std::mutex> rl(renderMu_);
        RenderLocked();
    }
}

void GlassWindow::EnsureSwapSize(int w, int h) {
    // Composition swap chains stretch their source region to the buffer size, so the buffers must
    // match the window exactly for 1:1 pixels.
    if (UINT(w) == bufW_ && UINT(h) == bufH_) return;
    ctx_->OMSetRenderTargets(0, nullptr, nullptr);
    rtv_.Reset();
    HRESULT hr = swap_->ResizeBuffers(2, UINT(w), UINT(h), DXGI_FORMAT_B8G8R8A8_UNORM,
                                      DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
    if (SUCCEEDED(hr)) {
        bufW_ = UINT(w);
        bufH_ = UINT(h);
    } else {
        LOGE("gpu: ResizeBuffers(%d, %d) failed 0x%08lx", w, h, hr);
    }
    ComPtr<ID3D11Texture2D> back;
    swap_->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(back.GetAddressOf()));
    dev_->CreateRenderTargetView(back.Get(), nullptr, rtv_.GetAddressOf());
}

void GlassWindow::ConvertVideoLocked() {
    convertedSeq_ = frameSeq_.load();
    int w = srcW_, h = srcH_;
    if (!srcKind_ || w <= 0 || h <= 0) return;
    if (!rgb_ || rgbW_ != w || rgbH_ != h) {
        rgb_.Reset();
        rgbSrv_.Reset();
        rgbRtv_.Reset();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = UINT(w);
        td.Height = UINT(h);
        td.MipLevels = 0;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, rgb_.GetAddressOf()))) return;
        dev_->CreateShaderResourceView(rgb_.Get(), nullptr, rgbSrv_.GetAddressOf());
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        dev_->CreateRenderTargetView(rgb_.Get(), &rd, rgbRtv_.GetAddressOf());
        rgbW_ = w;
        rgbH_ = h;
    }
    ConvertCB cb{};
    ColorMatrix(srcColor_, cb);
    Set4(cb.info, srcKind_ == 2 ? 1.0f : 0.0f, 0, 0, 0);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx_->Map(cbConvert_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        std::memcpy(m.pData, &cb, sizeof(cb));
        ctx_->Unmap(cbConvert_.Get(), 0);
    }
    ID3D11ShaderResourceView* srvs[4] = {};
    if (srcKind_ == 1) {
        srvs[0] = nv12Y_.Get();
        srvs[1] = nv12UV_.Get();
    } else if (srcKind_ == 2) {
        srvs[0] = swYSrv_.Get();
        srvs[2] = swUSrv_.Get();
        srvs[3] = swVSrv_.Get();
    } else {
        srvs[0] = swYSrv_.Get();
        srvs[1] = swUVSrv_.Get();
    }
    ID3D11RenderTargetView* rt = rgbRtv_.Get();
    ctx_->OMSetRenderTargets(1, &rt, nullptr);
    D3D11_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
    ctx_->RSSetViewports(1, &vp);
    ctx_->RSSetState(nullptr);
    ctx_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    ctx_->IASetInputLayout(nullptr);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->VSSetShader(vsFull_.Get(), nullptr, 0);
    ctx_->PSSetShader(psConvert_.Get(), nullptr, 0);
    ID3D11Buffer* cbs[] = {cbConvert_.Get()};
    ctx_->PSSetConstantBuffers(0, 1, cbs);
    ctx_->PSSetShaderResources(0, 4, srvs);
    ID3D11SamplerState* ss[] = {sampLinear_.Get()};
    ctx_->PSSetSamplers(0, 1, ss);
    ctx_->Draw(3, 0);
    ID3D11ShaderResourceView* none[4] = {};
    ctx_->PSSetShaderResources(0, 4, none);
    ctx_->OMSetRenderTargets(0, nullptr, nullptr);
    ctx_->GenerateMips(rgbSrv_.Get());
}

void GlassWindow::RenderLabelLocked(const std::wstring& name, float dp) {
    if (!d2d_ || !dwrite_) return;
    std::wstring title = name.empty() ? std::wstring(L"AirPlay") : name;
    std::wstring sub = L"Connecting…";
    ComPtr<IDWriteTextFormat> ft, fsub;
    dwrite_->CreateTextFormat(L"Segoe UI Variable Display", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                              DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 21.0f * dp, L"", ft.GetAddressOf());
    dwrite_->CreateTextFormat(L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                              DWRITE_FONT_STRETCH_NORMAL, 14.0f * dp, L"", fsub.GetAddressOf());
    if (!ft || !fsub) return;
    float maxW = 640.0f * dp;
    ComPtr<IDWriteTextLayout> lt, ls;
    dwrite_->CreateTextLayout(title.c_str(), UINT32(title.size()), ft.Get(), maxW, 400.0f * dp, lt.GetAddressOf());
    dwrite_->CreateTextLayout(sub.c_str(), UINT32(sub.size()), fsub.Get(), maxW, 400.0f * dp, ls.GetAddressOf());
    if (!lt || !ls) return;
    lt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    ls->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    ComPtr<IDWriteInlineObject> ellipsis;
    dwrite_->CreateEllipsisTrimmingSign(ft.Get(), ellipsis.GetAddressOf());
    lt->SetTrimming(&trim, ellipsis.Get());
    DWRITE_TEXT_METRICS mt{}, ms{};
    lt->GetMetrics(&mt);
    ls->GetMetrics(&ms);
    float gap = 5.0f * dp;
    float W = std::ceil(std::min(maxW, std::max(mt.widthIncludingTrailingWhitespace, ms.widthIncludingTrailingWhitespace)) +
                        12.0f * dp);
    float H = std::ceil(mt.height + gap + ms.height + 6.0f * dp);
    lt->SetMaxWidth(W);
    ls->SetMaxWidth(W);
    lt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    ls->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);

    label_.Reset();
    labelSrv_.Reset();
    D3D11_TEXTURE2D_DESC td{};
    td.Width = UINT(W);
    td.Height = UINT(H);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev_->CreateTexture2D(&td, nullptr, label_.GetAddressOf()))) return;
    dev_->CreateShaderResourceView(label_.Get(), nullptr, labelSrv_.GetAddressOf());
    ComPtr<IDXGISurface> surf;
    label_.As(surf);
    D2D1_RENDER_TARGET_PROPERTIES props{};
    props.type = D2D1_RENDER_TARGET_TYPE_DEFAULT;
    props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    props.dpiX = props.dpiY = 96.0f;
    ComPtr<ID2D1RenderTarget> rt;
    if (FAILED(d2d_->CreateDxgiSurfaceRenderTarget(surf.Get(), &props, rt.GetAddressOf()))) return;
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    ComPtr<ID2D1SolidColorBrush> white, dim;
    rt->CreateSolidColorBrush(D2D1_COLOR_F{1, 1, 1, 1}, white.GetAddressOf());
    rt->CreateSolidColorBrush(D2D1_COLOR_F{1, 1, 1, 0.72f}, dim.GetAddressOf());
    rt->BeginDraw();
    rt->Clear(D2D1_COLOR_F{0, 0, 0, 0});
    rt->DrawTextLayout(D2D1_POINT_2F{0, 2.0f * dp}, lt.Get(), white.Get());
    rt->DrawTextLayout(D2D1_POINT_2F{0, 2.0f * dp + mt.height + gap}, ls.Get(), dim.Get());
    rt->EndDraw();
    labelW_ = int(W);
    labelH_ = int(H);
}

void GlassWindow::RenderLocked() {
    Shared s;
    {
        std::lock_guard<std::mutex> lk(stateMu_);
        s = shared_;
        shared_.snap = false;
        shared_.resetAnim = false;
        shared_.labelDirty = false;
    }
    if (!s.visible || s.iconic || s.winW <= 0 || s.winH <= 0 || !swap_ || deviceLost_) return;

    double now = NowSeconds();
    if (anim_.t0 == 0) anim_.t0 = now;
    float dt = anim_.lastTime == 0 ? 1.0f / 120.0f : float(std::clamp(now - anim_.lastTime, 0.0, 0.05));
    anim_.lastTime = now;

    if (s.resetAnim) {
        anim_.x0 = {s.slabTarget.x0, 0};
        anim_.y0 = {s.slabTarget.y0, 0};
        anim_.x1 = {s.slabTarget.x1, 0};
        anim_.y1 = {s.slabTarget.y1, 0};
        anim_.appear = {0, 0};
        anim_.fs = {s.fsTarget, 0};
        anim_.controls = {0, 0};
        anim_.dropA = {0, 0};
        anim_.press = {0, 0};
        anim_.reveal = {0, 0};
    }
    if (s.snap) {
        anim_.x0 = {s.slabTarget.x0, 0};
        anim_.y0 = {s.slabTarget.y0, 0};
        anim_.x1 = {s.slabTarget.x1, 0};
        anim_.y1 = {s.slabTarget.y1, 0};
    }
    const float kSlab = 170.0f, zSlab = 0.86f;
    StepSpring(anim_.x0.x, anim_.x0.v, s.slabTarget.x0, kSlab, zSlab, dt);
    StepSpring(anim_.y0.x, anim_.y0.v, s.slabTarget.y0, kSlab, zSlab, dt);
    StepSpring(anim_.x1.x, anim_.x1.v, s.slabTarget.x1, kSlab, zSlab, dt);
    StepSpring(anim_.y1.x, anim_.y1.v, s.slabTarget.y1, kSlab, zSlab, dt);
    StepSpring(anim_.appear.x, anim_.appear.v, s.appearTarget, 260.0f, s.appearTarget > 0.5f ? 0.66f : 1.0f, dt);
    StepSpring(anim_.fs.x, anim_.fs.v, s.fsTarget, 170.0f, 0.95f, dt);
    StepSpring(anim_.controls.x, anim_.controls.v, s.controlsTarget, 380.0f, 0.82f, dt);
    bool hasVideo = frameSeq_.load() > s.sessionStartSeq;
    StepSpring(anim_.reveal.x, anim_.reveal.v, hasVideo ? 1.0f : 0.0f, 90.0f, 1.0f, dt);

    RectF slab{anim_.x0.x, anim_.y0.x, anim_.x1.x, anim_.y1.x};
    float fs = std::clamp(anim_.fs.x, 0.0f, 1.0f);
    Layout L = ComputeLayout(slab, fs, s.dp, s.aspect, s.tv ? 4 : 3, s.tv ? s.tvBoost : 1.0f);

    bool hovering = s.hoverBtn >= 0 && s.hoverBtn < L.buttons && s.controlsTarget > 0.5f;
    float dropTarget = hovering ? L.btnX[s.hoverBtn] : anim_.dropX.x;
    if (anim_.dropA.x < 0.05f && hovering) anim_.dropX = {dropTarget, 0};
    StepSpring(anim_.dropX.x, anim_.dropX.v, dropTarget, 520.0f, 0.70f, dt);
    StepSpring(anim_.dropA.x, anim_.dropA.v, hovering ? 1.0f : 0.0f, 420.0f, 0.9f, dt);
    StepSpring(anim_.press.x, anim_.press.v, s.pressing ? 1.0f : 0.0f, 900.0f, 0.75f, dt);

    {
        std::lock_guard<std::mutex> lk(stateMu_);
        pubSlab_ = slab;
        pubLayout_ = L;
        pubControls_ = anim_.controls.x;
    }

    ComposeCB cb{};
    float W = float(s.winW), H = float(s.winH);
    Set4(cb.view, W, H, 1.0f / W, 1.0f / H);
    Set4(cb.slab, slab.x0, slab.y0, slab.x1, slab.y1);
    Set4(cb.video, L.video.x0, L.video.y0, L.video.x1, L.video.y1);
    Set4(cb.geom, L.radius, L.bezel, s.dp, fs);
    float ap = std::clamp(anim_.appear.x, 0.0f, 1.2f);
    Set4(cb.appear, 0.92f + 0.08f * ap, std::clamp(ap * 1.25f, 0.0f, 1.0f), (1.0f - std::min(ap, 1.0f)) * 16.0f * s.dp,
         float(now - anim_.t0));
    Set4(cb.shadow, kShadowOffsetDp * s.dp, kShadowSoftDp * s.dp, kShadowAlpha, 0);
    float rv = std::clamp(anim_.reveal.x, 0.0f, 1.0f);
    Set4(cb.vinfo, float(std::max(rgbW_, 1)), float(std::max(rgbH_, 1)), rgbSrv_ ? rv : 0.0f, (1.0f - rv) * 6.0f);
    Set4(cb.pill, L.pill.x0, L.pill.y0, L.pill.x1, L.pill.y1);
    Set4(cb.pillInfo, std::clamp(anim_.controls.x, 0.0f, 1.0f), L.uiScale, 1.0f - rv, L.btnR);
    float press = std::clamp(anim_.press.x, 0.0f, 1.0f);
    float dropA = std::clamp(anim_.dropA.x, 0.0f, 1.0f);
    Set4(cb.drop, anim_.dropX.x, L.btnY, L.btnR * (1.10f - 0.12f * press), dropA);
    Set4(cb.btnX, L.btnX[0], L.btnX[1], L.btnX[2], L.btnY);
    float hv[4];
    for (int i = 0; i < 4; ++i)
        hv[i] = std::clamp(1.0f - std::fabs(anim_.dropX.x - L.btnX[i]) / (L.spacing * 0.7f), 0.0f, 1.0f) * dropA;
    Set4(cb.btnState, hv[0], hv[1], hv[2], press);
    Set4(cb.icons, s.fsTarget > 0.5f ? 1.0f : 0.0f, s.pinned ? 1.0f : 0.0f, L.iconHalf, L.stroke);
    Set4(cb.tv0, L.btnX[3], hv[3], s.tv ? 1.0f : 0.0f, 0);
    Set4(cb.tv1, float(s.tvCorner), float(s.tvSize), 0, 0);
    float gs = std::min(L.video.W(), L.video.H()) * 0.12f;
    float gcx = L.video.CX(), gcy = L.video.CY() - L.video.H() * 0.05f;

    {
        std::lock_guard<std::recursive_mutex> g(gpuLock_);
        EnsureSwapSize(s.winW, s.winH);
        if (frameSeq_.load() != convertedSeq_) ConvertVideoLocked();
        if (s.labelDirty || !label_) RenderLabelLocked(s.labelName, s.dp);
        float lw = float(labelW_), lh = float(labelH_);
        float ls = std::min(1.0f, L.video.W() * 0.86f / std::max(lw, 1.0f));
        float ly = gcy + gs * 1.45f;
        Set4(cb.label, gcx - lw * ls * 0.5f, ly, gcx + lw * ls * 0.5f, ly + lh * ls);
        Set4(cb.labelInfo, labelSrv_ ? 1.0f : 0.0f, gcx, gcy, gs);

        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx_->Map(cbCompose_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            std::memcpy(m.pData, &cb, sizeof(cb));
            ctx_->Unmap(cbCompose_.Get(), 0);
        }
        ID3D11RenderTargetView* rt = rtv_.Get();
        ctx_->OMSetRenderTargets(1, &rt, nullptr);
        D3D11_VIEWPORT vp{0, 0, W, H, 0, 1};
        ctx_->RSSetViewports(1, &vp);
        ctx_->RSSetState(nullptr);
        ctx_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        ctx_->OMSetDepthStencilState(nullptr, 0);
        ctx_->IASetInputLayout(nullptr);
        ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx_->VSSetShader(vsFull_.Get(), nullptr, 0);
        ctx_->PSSetShader(psCompose_.Get(), nullptr, 0);
        ID3D11Buffer* cbs[] = {cbCompose_.Get()};
        ctx_->PSSetConstantBuffers(0, 1, cbs);
        ID3D11ShaderResourceView* srvs[] = {rgbSrv_.Get(), labelSrv_.Get()};
        ctx_->PSSetShaderResources(0, 2, srvs);
        ID3D11SamplerState* ss[] = {sampLinear_.Get()};
        ctx_->PSSetSamplers(0, 1, ss);
        ctx_->Draw(3, 0);
        ID3D11ShaderResourceView* none[2] = {};
        ctx_->PSSetShaderResources(0, 2, none);
        if (!snapDir_.empty() && snapNext_ < snapTimes_.size() && now - sessionStart_ >= snapTimes_[snapNext_]) {
            SaveSnapshotLocked(int(snapNext_));
            ++snapNext_;
        }
        HRESULT hr = swap_->Present(1, 0);
        if (FAILED(hr) && !deviceLost_) {
            deviceLost_ = true;
            LOGE("gpu: Present failed 0x%08lx (device removed reason 0x%08lx); restart AirGlass to recover", hr,
                 dev_->GetDeviceRemovedReason());
        }
    }
    if (!snapDir_.empty() && snapNext_ < snapTimes_.size()) needFrame_ = true;

    bool slabDone = Near(anim_.x0.x, anim_.x0.v, s.slabTarget.x0, 0.35f) &&
                    Near(anim_.y0.x, anim_.y0.v, s.slabTarget.y0, 0.35f) &&
                    Near(anim_.x1.x, anim_.x1.v, s.slabTarget.x1, 0.35f) &&
                    Near(anim_.y1.x, anim_.y1.v, s.slabTarget.y1, 0.35f);
    bool fsDone = Near(anim_.fs.x, anim_.fs.v, s.fsTarget, 0.002f);
    bool done = slabDone && fsDone && Near(anim_.appear.x, anim_.appear.v, s.appearTarget, 0.002f) &&
                Near(anim_.controls.x, anim_.controls.v, s.controlsTarget, 0.002f) &&
                Near(anim_.dropX.x, anim_.dropX.v, dropTarget, 0.3f) &&
                Near(anim_.dropA.x, anim_.dropA.v, hovering ? 1.0f : 0.0f, 0.002f) &&
                Near(anim_.press.x, anim_.press.v, s.pressing ? 1.0f : 0.0f, 0.002f) &&
                Near(anim_.reveal.x, anim_.reveal.v, hasVideo ? 1.0f : 0.0f, 0.002f) && hasVideo;
    if (!done) needFrame_ = true;
    if (slabDone) {
        anim_.x0 = {s.slabTarget.x0, 0};
        anim_.y0 = {s.slabTarget.y0, 0};
        anim_.x1 = {s.slabTarget.x1, 0};
        anim_.y1 = {s.slabTarget.y1, 0};
    }
    std::lock_guard<std::mutex> lk(stateMu_);
    if (shared_.settleRequest && slabDone && fsDone) {
        shared_.settleRequest = false;
        PostMessageW(hwnd_, kMsgSettled, 0, 0);
    }
    if (shared_.dismissRequest && shared_.appearTarget == 0.0f && anim_.appear.x < 0.02f) {
        shared_.dismissRequest = false;
        PostMessageW(hwnd_, kMsgDismissed, 0, 0);
    }
}

// Writes the current back buffer, composited over a neutral checkerboard, as a 24-bit BMP.
void GlassWindow::SaveSnapshotLocked(int index) {
    ComPtr<ID3D11Texture2D> back;
    swap_->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(back.GetAddressOf()));
    D3D11_TEXTURE2D_DESC d;
    back->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(dev_->CreateTexture2D(&d, nullptr, staging.GetAddressOf()))) return;
    ctx_->CopyResource(staging.Get(), back.Get());
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return;
    int w = int(d.Width), h = int(d.Height);
    int stride = (w * 3 + 3) & ~3;
    std::vector<uint8_t> px(size_t(stride) * h);
    for (int y = 0; y < h; ++y) {
        const uint8_t* src = static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch;
        uint8_t* dst = px.data() + size_t(h - 1 - y) * stride;
        for (int x = 0; x < w; ++x) {
            bool check = ((x / 16) + (y / 16)) & 1;
            float bg = check ? 0.30f : 0.36f;  // neutral backdrop so shadow/transparency are visible
            float a = src[4 * x + 3] / 255.0f;
            for (int c = 0; c < 3; ++c) dst[3 * x + c] = uint8_t(std::min(255.0f, src[4 * x + c] + bg * 255.0f * (1 - a)));
        }
    }
    ctx_->Unmap(staging.Get(), 0);
    wchar_t name[64];
    swprintf(name, 64, L"\\snap_%02d.bmp", index);
    FILE* f = _wfopen((snapDir_ + name).c_str(), L"wb");
    if (!f) return;
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof(ih);
    ih.biWidth = w;
    ih.biHeight = h;
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    ih.biSizeImage = DWORD(px.size());
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = DWORD(fh.bfOffBits + px.size());
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    fwrite(px.data(), 1, px.size(), f);
    fclose(f);
    LOGI("debug: snapshot %d (%dx%d) at t=%.2fs", index, w, h, NowSeconds() - sessionStart_);
}

// ---------------------------------------------------------------------------------------------
// Geometry (UI thread)

RectF GlassWindow::CurrentSlabScreen() {
    std::lock_guard<std::mutex> lk(stateMu_);
    return pubSlab_.Offset(float(winRect_.left), float(winRect_.top));
}

RectF GlassWindow::WorkArea(HMONITOR mon) const {
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    return RectF::FromRECT(mi.rcWork);
}

RectF GlassWindow::ClampToWork(RectF r, HMONITOR mon) const {
    RectF wa = WorkArea(mon).Inflate(-8.0f * dp_);
    float w = r.W(), h = r.H();
    float scale = std::min({1.0f, wa.W() / std::max(w, 1.0f), wa.H() / std::max(h, 1.0f)});
    if (scale < 1.0f) {
        float cx = r.CX(), cy = r.CY();
        w *= scale;
        h *= scale;
        r = {cx - w * 0.5f, cy - h * 0.5f, cx + w * 0.5f, cy + h * 0.5f};
    }
    float dx = 0, dy = 0;
    if (r.x0 < wa.x0) dx = wa.x0 - r.x0;
    if (r.x1 > wa.x1) dx = wa.x1 - r.x1;
    if (r.y0 < wa.y0) dy = wa.y0 - r.y0;
    if (r.y1 > wa.y1) dy = wa.y1 - r.y1;
    r = r.Offset(dx, dy);
    return {std::round(r.x0), std::round(r.y0), std::round(r.x0) + std::round(r.W()), std::round(r.y0) + std::round(r.H())};
}

RectF GlassWindow::InitialSlab(HMONITOR mon, float aspect) const {
    RectF wa = WorkArea(mon);
    bool portrait = aspect < 1.0f;
    float longSide = float(portrait ? opt_.longPortrait : opt_.longLandscape);
    if (longSide <= 0) longSide = portrait ? wa.H() * 0.62f : std::min(wa.W() * 0.55f, wa.H() * 0.70f * aspect);
    float w = portrait ? longSide * aspect : longSide;
    float h = portrait ? longSide : longSide / aspect;
    RectF r{wa.CX() - w * 0.5f, wa.CY() - h * 0.5f, wa.CX() + w * 0.5f, wa.CY() + h * 0.5f};
    return ClampToWork(r, mon);
}

RectF GlassWindow::SlabForAspect(const RectF& cur, float aspect) const {
    bool portrait = aspect < 1.0f;
    float longSide = float(portrait ? opt_.longPortrait : opt_.longLandscape);
    if (longSide <= 0) longSide = std::max(cur.W(), cur.H());
    float w = portrait ? longSide * aspect : longSide;
    float h = portrait ? longSide : longSide / aspect;
    RectF r{cur.CX() - w * 0.5f, cur.CY() - h * 0.5f, cur.CX() + w * 0.5f, cur.CY() + h * 0.5f};
    HMONITOR mon = MonitorFromRect(&winRect_, MONITOR_DEFAULTTONEAREST);
    return ClampToWork(r, mon);
}

void GlassWindow::SetWindowRectSync(const RectF& winScreen) {
    std::lock_guard<std::mutex> rl(renderMu_);
    RECT nr = winScreen.ToRECT();
    float dx = float(nr.left - winRect_.left), dy = float(nr.top - winRect_.top);
    {
        std::lock_guard<std::mutex> lk(stateMu_);
        shared_.slabTarget = shared_.slabTarget.Offset(-dx, -dy);
        shared_.winW = nr.right - nr.left;
        shared_.winH = nr.bottom - nr.top;
        pubSlab_ = pubSlab_.Offset(-dx, -dy);
    }
    anim_.x0.x -= dx;
    anim_.x1.x -= dx;
    anim_.y0.x -= dy;
    anim_.y1.x -= dy;
    winRect_ = nr;
    internalMove_ = true;
    SetWindowPos(hwnd_, nullptr, nr.left, nr.top, nr.right - nr.left, nr.bottom - nr.top,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    internalMove_ = false;
    if (frameWait_) WaitForSingleObjectEx(frameWait_, 20, FALSE);
    RenderLocked();
}

void GlassWindow::AnimateSlabTo(const RectF& target) {
    RectF cur = CurrentSlabScreen();
    RectF uni = RectF::Union(cur, target).Inflate(Margin());
    RECT u = uni.ToRECT();
    if (!SameRect(u, winRect_)) SetWindowRectSync(uni);
    {
        std::lock_guard<std::mutex> lk(stateMu_);
        shared_.slabTarget = target.Offset(-float(winRect_.left), -float(winRect_.top));
        shared_.settleRequest = true;
    }
    animTarget_ = target;
    hasAnimTarget_ = true;
    pendingShrink_ = true;
    shrinkTo_ = target.Inflate(Margin());
    Wake();
}

void GlassWindow::OnExternalResize() {
    std::lock_guard<std::mutex> rl(renderMu_);
    GetWindowRect(hwnd_, &winRect_);
    int w = winRect_.right - winRect_.left, h = winRect_.bottom - winRect_.top;
    RectF slab;
    if (fullscreen_) {
        slab = {0, 0, float(w), float(h)};
    } else {
        float m = Margin();
        slab = FitAspect(RectF{m, m, float(w) - m, float(h) - m}, aspect_);
        slab = {std::round(slab.x0), std::round(slab.y0), std::round(slab.x1), std::round(slab.y1)};
    }
    {
        std::lock_guard<std::mutex> lk(stateMu_);
        shared_.winW = w;
        shared_.winH = h;
        shared_.slabTarget = slab;
        shared_.snap = true;
        pubSlab_ = slab;
    }
    anim_.x0 = {slab.x0, 0};
    anim_.y0 = {slab.y0, 0};
    anim_.x1 = {slab.x1, 0};
    anim_.y1 = {slab.y1, 0};
    pendingShrink_ = false;
    hasAnimTarget_ = false;
    if (visible_) {
        if (frameWait_) WaitForSingleObjectEx(frameWait_, 20, FALSE);
        RenderLocked();
    }
}

void GlassWindow::EnterFullscreen(HMONITOR mon) {
    if (fullscreen_ || !visible_) return;
    restoreSlab_ = hasAnimTarget_ ? animTarget_ : CurrentSlabScreen();
    if (!mon) mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    pendingShrink_ = false;
    hasAnimTarget_ = false;
    fullscreen_ = true;
    SetWindowRectSync(RectF::FromRECT(mi.rcMonitor));
    {
        std::lock_guard<std::mutex> lk(stateMu_);
        shared_.slabTarget = {0, 0, float(mi.rcMonitor.right - mi.rcMonitor.left),
                              float(mi.rcMonitor.bottom - mi.rcMonitor.top)};
        shared_.fsTarget = 1.0f;
        shared_.settleRequest = false;
    }
    if (snapDir_.empty() && !tv_) SetForegroundWindow(hwnd_);  // TV Mode keeps the focus
    Wake();
}

void GlassWindow::ExitFullscreen() {
    if (!fullscreen_) return;
    if (!pro_ && sessionLive_ && !dismissing_) {
        LOGI("ui: windowed mode needs AirGlass Pro");
        if (onUpgradeRequested) onUpgradeRequested();
        return;
    }
    fullscreen_ = false;
    RectF target = restoreSlab_;
    if (tv_ && !tvFree_) target = TvFloatSlab(tvLayout_, aspect_);
    {
        std::lock_guard<std::mutex> lk(stateMu_);
        shared_.slabTarget = target.Offset(-float(winRect_.left), -float(winRect_.top));
        shared_.fsTarget = 0.0f;
        shared_.settleRequest = true;
    }
    animTarget_ = target;
    hasAnimTarget_ = true;
    pendingShrink_ = true;
    shrinkTo_ = target.Inflate(Margin());
    Wake();
}

void GlassWindow::SetPinned(bool pinned) {
    pinned_ = pinned;
    opt_.pinned = pinned;
    SetWindowPos(hwnd_, pinned ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    {
        std::lock_guard<std::mutex> lk(stateMu_);
        shared_.pinned = pinned;
    }
    Wake();
    if (onOptionsChanged) onOptionsChanged();
}

void GlassWindow::RememberSize() {
    if (fullscreen_) return;
    RectF s = hasAnimTarget_ ? animTarget_ : CurrentSlabScreen();
    int longSide = int(std::lround(std::max(s.W(), s.H())));
    if (longSide <= 0) return;
    if (aspect_ < 1.0f)
        opt_.longPortrait = longSide;
    else
        opt_.longLandscape = longSide;
    if (onOptionsChanged) onOptionsChanged();
}

// ---------------------------------------------------------------------------------------------
// Session lifecycle

void GlassWindow::BeginSession(uint64_t sid, const std::wstring& deviceName, const std::string& model) {
    sid_ = sid;
    sessionLive_ = true;
    bool keepHiding = visible_ && tvHiding_ && tv_ && tvDefaults_.mode == TvLayout::Hidden;
    {
        std::lock_guard<std::mutex> lk(stateMu_);
        shared_.labelName = deviceName.empty() ? L"AirPlay" : deviceName;
        shared_.labelDirty = true;
        shared_.sessionStartSeq = frameSeq_.load();
        if (!keepHiding) {
            shared_.appearTarget = 1.0f;
            shared_.dismissRequest = false;
        }
    }
    if (visible_) {  // a new sender took over the window (possibly while it was melting away)
        if (!keepHiding) {
            dismissing_ = false;
            tvHiding_ = false;
        } else {
            dismissing_ = false;  // keep melting away, but only into "hidden": the session continues
        }
        if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
        Wake();
        return;
    }
    dismissing_ = false;
    aspect_ = GuessAspect(model);
    tvFree_ = false;
    sessionStart_ = NowSeconds();  // debug snapshot timeline
    snapNext_ = 0;
    if (tv_) {
        tvLayout_ = tvDefaults_;
        if (tvLayout_.mode == TvLayout::Hidden) {
            tvHidden_ = true;
            LOGI("ui: session %llu starts hidden (TV Mode shows a notification instead)", (unsigned long long)sid);
            return;
        }
    }
    tvHidden_ = false;
    ShowWindowForSession();
    RECT wr;
    GetWindowRect(hwnd_, &wr);
    LOGI("ui: window %p shown for session %llu (%s) visible=%d rect=%ld,%ld %ldx%ld%s", (void*)hwnd_,
         (unsigned long long)sid, WideToUtf8(deviceName).c_str(), IsWindowVisible(hwnd_), wr.left, wr.top,
         wr.right - wr.left, wr.bottom - wr.top, tv_ ? " [TV Mode]" : "");
}

void GlassWindow::ShowWindowForSession() {
    HMONITOR mon;
    if (tv_) {
        mon = TvMonitor(nullptr);
    } else {
        POINT cursor;
        GetCursorPos(&cursor);
        mon = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    }
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
    dp_ = float(dx) / 96.0f;
    fullscreen_ = false;
    pendingShrink_ = false;
    hasAnimTarget_ = false;
    tvHidden_ = false;
    RectF slab = tv_ ? TvFloatSlab(tvLayout_, aspect_) : InitialSlab(mon, aspect_);
    {
        std::lock_guard<std::mutex> lk(stateMu_);
        shared_.aspect = aspect_;
        shared_.dp = dp_;
        shared_.fsTarget = 0.0f;
        shared_.controlsTarget = 0.0f;
        shared_.hoverBtn = -1;
        shared_.pressing = false;
        shared_.resetAnim = true;
        shared_.visible = true;
        shared_.iconic = false;
        shared_.appearTarget = 1.0f;
        shared_.dismissRequest = false;
        shared_.tv = tv_;
        shared_.tvCorner = tvLayout_.corner;
        shared_.tvSize = tvLayout_.size;
        shared_.slabTarget = slab.Offset(-float(winRect_.left), -float(winRect_.top));
        pubSlab_ = shared_.slabTarget;
    }
    UpdateTvBoost();
    SetWindowRectSync(slab.Inflate(Margin()));
    if (snapDir_.empty()) {
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        if (!pinned_ && !tv_) SetWindowPos(hwnd_, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    visible_ = true;
    SetTimer(hwnd_, kTimerIdle, 100, nullptr);
    if (debugHover_ >= 0) {
        std::lock_guard<std::mutex> lk(stateMu_);
        shared_.controlsTarget = 1.0f;
        shared_.hoverBtn = std::min(debugHover_, tv_ ? 3 : 2);
    }
    if (debugFsIn_ > 0) SetTimer(hwnd_, kTimerDebugFsIn, UINT(debugFsIn_ * 1000), nullptr);
    if (debugFsOut_ > 0) SetTimer(hwnd_, kTimerDebugFsOut, UINT(debugFsOut_ * 1000), nullptr);
    if ((tv_ && tvLayout_.mode == TvLayout::Full) || !pro_) EnterFullscreen(mon);
    if (tvHighlight_ >= 0) {
        std::lock_guard<std::mutex> lk(stateMu_);
        shared_.hoverBtn = tvHighlight_;
    }
    UpdateControls();
    Wake();
}

void GlassWindow::EndSession(uint64_t sid) {
    if (sid != sid_ || !sessionLive_) return;
    sessionLive_ = false;
    tvHighlight_ = -1;
    if (!visible_ || tvHidden_) {
        tvHidden_ = false;
        tvHiding_ = false;
        LOGI("ui: session %llu ended (window was hidden)", (unsigned long long)sid);
        return;
    }
    tvHiding_ = false;
    dismissing_ = true;
    {
        std::lock_guard<std::mutex> lk(stateMu_);
        shared_.appearTarget = 0.0f;
        shared_.controlsTarget = 0.0f;
        shared_.dismissRequest = true;
        if (shared_.iconic) PostMessageW(hwnd_, kMsgDismissed, 0, 0);
    }
    LOGI("ui: dismissing session %llu", (unsigned long long)sid);
    Wake();
}

void GlassWindow::SetVideoSize(uint64_t sid, int w, int h) {
    if (sid != sid_ || w <= 0 || h <= 0 || !sessionLive_) return;
    float a = float(w) / float(h);
    bool changed = std::fabs(a - aspect_) > 0.004f;
    aspect_ = a;
    {
        std::lock_guard<std::mutex> lk(stateMu_);
        shared_.aspect = a;
    }
    if (changed && visible_ && !tvHiding_) {
        if (fullscreen_) {
            restoreSlab_ = SlabForAspect(restoreSlab_, a);
        } else if (tv_ && !tvFree_) {
            AnimateSlabTo(TvFloatSlab(tvLayout_, a));  // rotation keeps the TV corner
        } else {
            RectF cur = hasAnimTarget_ ? animTarget_ : CurrentSlabScreen();
            AnimateSlabTo(SlabForAspect(cur, a));
        }
    }
    Wake();
}

void GlassWindow::BringToFront() {
    if (!visible_) return;
    if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
    SetForegroundWindow(hwnd_);
}

// ---------------------------------------------------------------------------------------------
// TV Mode

HMONITOR GlassWindow::TvMonitor(RectF* area) const {
    // TV Mode (or the app it launched) is the foreground window; float over its screen.
    HWND fg = GetForegroundWindow();
    wchar_t cls[64] = L"";
    if (fg) GetClassNameW(fg, cls, 64);
    bool shell = !fg || fg == hwnd_ || wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0 ||
                 wcscmp(cls, L"Shell_TrayWnd") == 0;
    HMONITOR mon = !shell ? MonitorFromWindow(fg, MONITOR_DEFAULTTOPRIMARY)
                          : visible_ ? MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY)
                                     : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    if (area) {
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(mon, &mi);
        // A full-screen app covers the taskbar, so the whole screen is usable then.
        RECT r{};
        bool fgFull = !shell && GetWindowRect(fg, &r) && r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top &&
                      r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom;
        *area = RectF::FromRECT(fgFull ? mi.rcMonitor : mi.rcWork);
    }
    return mon;
}

void GlassWindow::UpdateTvBoost() {
    float boost = 1.0f;
    if (tv_) {
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(TvMonitor(nullptr), &mi);
        boost = std::max(1.0f, float(mi.rcMonitor.bottom - mi.rcMonitor.top) / (1080.0f * dp_));
    }
    std::lock_guard<std::mutex> lk(stateMu_);
    shared_.tvBoost = boost;
}

RectF GlassWindow::TvFloatSlab(const TvLayout& l, float aspect) const {
    RectF area;
    TvMonitor(&area);
    static const float kWidth[3] = {0.28f, 0.40f, 0.55f};  // of the screen width, for 16:9 content
    static const float kMaxH[3] = {0.62f, 0.80f, 1.0f};    // tall (portrait) content stays out of the way
    int sz = std::clamp(l.size, 0, 2);
    float m = float(std::clamp(l.margin, 0, 400));
    float a = std::max(aspect, 0.05f);
    // Same screen area for every shape: a portrait phone gets as much glass as a 16:9 picture.
    float ref = kWidth[sz] * area.W();
    float A = ref * ref * 9.0f / 16.0f;
    float w = std::sqrt(A * a), h = std::sqrt(A / a);
    float k = std::min({1.0f, (area.W() - 2.0f * m) / w, (area.H() - 2.0f * m) * kMaxH[sz] / h});
    w *= k;
    h *= k;
    bool left = l.corner == 1 || l.corner == 3, top = l.corner >= 2;
    float x0 = std::round(left ? area.x0 + m : area.x1 - m - w);
    float y0 = std::round(top ? area.y0 + m : area.y1 - m - h);
    return {x0, y0, x0 + std::round(w), y0 + std::round(h)};
}

void GlassWindow::SetTvControlled(bool on) {
    if (tv_ == on) return;
    tv_ = on;
    LOGI("ui: %s", on ? "TV Mode connected - window floats over it and never takes focus"
                      : "TV Mode disconnected - standalone window");
    LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, on ? (ex | WS_EX_NOACTIVATE) : (ex & ~LONG_PTR(WS_EX_NOACTIVATE)));
    tvHighlight_ = -1;
    {
        std::lock_guard<std::mutex> lk(stateMu_);
        shared_.tv = on;
        shared_.hoverBtn = hoverBtn_;
        shared_.tvCorner = tvLayout_.corner;
        shared_.tvSize = tvLayout_.size;
    }
    UpdateTvBoost();
    if (!on && sessionLive_) {
        // Standalone has no hidden mode: bring the picture back.
        if (tvHiding_) {
            tvHiding_ = false;
            std::lock_guard<std::mutex> lk(stateMu_);
            shared_.appearTarget = 1.0f;
            shared_.dismissRequest = false;
        } else if (tvHidden_) {
            ShowWindowForSession();
        }
    }
    if (visible_)
        SetWindowPos(hwnd_, (on || pinned_) ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    UpdateControls();
    Wake();
}

void GlassWindow::ApplyTvLayout(const TvLayout& l, bool byUser) {
    if (!sessionLive_) return;
    // TV Mode re-sends the layout after every session start: a repeat only confirms it.
    bool same = l.mode == tvLayout_.mode && l.corner == tvLayout_.corner && l.size == tvLayout_.size &&
                l.margin == tvLayout_.margin && !tvFree_;
    bool inPlace = l.mode == TvLayout::Hidden ? (tvHidden_ || tvHiding_)
                   : l.mode == TvLayout::Full ? (visible_ && !tvHiding_ && fullscreen_)
                                              : (visible_ && !tvHiding_ && !fullscreen_);
    if (same && inPlace) {
        if (onTvLayoutChanged) onTvLayoutChanged(byUser);
        return;
    }
    tvLayout_ = l;
    tvFree_ = false;
    {
        std::lock_guard<std::mutex> lk(stateMu_);
        shared_.tvCorner = l.corner;
        shared_.tvSize = l.size;
    }
    if (l.mode == TvLayout::Hidden) {
        HideForTv();
    } else {
        if (tvHiding_) {  // changed its mind while melting away
            tvHiding_ = false;
            std::lock_guard<std::mutex> lk(stateMu_);
            shared_.appearTarget = 1.0f;
            shared_.dismissRequest = false;
        }
        if (!visible_) {
            ShowWindowForSession();
        } else if (l.mode == TvLayout::Full) {
            if (!fullscreen_) EnterFullscreen(TvMonitor(nullptr));
        } else if (fullscreen_) {
            ExitFullscreen();  // lands on the TV corner
        } else {
            AnimateSlabTo(TvFloatSlab(l, aspect_));
        }
        if (visible_ && tv_ && snapDir_.empty())
            SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    LOGI("ui: TV layout %s corner %d size %d%s", TvModeName(), l.corner, l.size, byUser ? " (by the user)" : "");
    Wake();
    if (onTvLayoutChanged) onTvLayoutChanged(byUser);
}

void GlassWindow::HideForTv() {
    if (!visible_ || dismissing_) {
        tvHidden_ = sessionLive_;
        return;
    }
    tvHiding_ = true;
    std::lock_guard<std::mutex> lk(stateMu_);
    shared_.appearTarget = 0.0f;
    shared_.controlsTarget = 0.0f;
    shared_.dismissRequest = true;
    if (shared_.iconic) PostMessageW(hwnd_, kMsgDismissed, 0, 0);
}

void GlassWindow::SetTvHighlight(int button) {
    if (button < -1 || button > kTvStop) button = -1;
    tvHighlight_ = tv_ ? button : -1;  // kept while hidden; shown when the window appears
    {
        std::lock_guard<std::mutex> lk(stateMu_);
        shared_.hoverBtn = hoverBtn_ >= 0 ? hoverBtn_ : tvHighlight_;
    }
    UpdateControls();
    Wake();
}

void GlassWindow::PressTvButton(int button) {
    if (!tv_ || !visible_ || button < 0 || button > kTvStop) return;
    tvHighlight_ = button;
    {
        std::lock_guard<std::mutex> lk(stateMu_);
        shared_.hoverBtn = button;
        shared_.pressing = true;
    }
    UpdateControls();
    SetTimer(hwnd_, kTimerTvPress, 160, nullptr);
    Wake();
}

void GlassWindow::TvButtonClicked(int b) {
    TvLayout l = tvLayout_;
    switch (b) {
    case kTvFull: l.mode = fullscreen_ ? TvLayout::Float : TvLayout::Full; break;
    case kTvCorner: {
        static const int kNext[4] = {1, 3, 0, 2};  // br -> bl -> tl -> tr -> br
        l.corner = kNext[std::clamp(l.corner, 0, 3)];
        l.mode = TvLayout::Float;
        break;
    }
    case kTvSize:
        l.size = (std::clamp(l.size, 0, 2) + 1) % 3;
        l.mode = TvLayout::Float;
        break;
    case kTvStop:
        LOGI("ui: stop pressed");
        if (onUserClose) onUserClose();
        return;
    default: return;
    }
    ApplyTvLayout(l, true);
}

const char* GlassWindow::TvModeName() const {
    if (tvHidden_ || tvHiding_) return "hidden";
    if (fullscreen_) return "full";
    if (tvFree_) return "free";
    return "float";
}

RECT GlassWindow::TvRect() {
    if (!visible_ || tvHiding_) return RECT{0, 0, 0, 0};
    if (fullscreen_) return winRect_;
    RectF s = hasAnimTarget_ ? animTarget_ : CurrentSlabScreen();
    return s.ToRECT();
}

// ---------------------------------------------------------------------------------------------
// Input

LRESULT GlassWindow::HitTest(int sx, int sy) {
    if (fullscreen_) return HTCLIENT;
    float x = float(sx - winRect_.left), y = float(sy - winRect_.top);
    RectF s;
    {
        std::lock_guard<std::mutex> lk(stateMu_);
        s = pubSlab_;
    }
    float band = 8.0f * dp_, corner = 24.0f * dp_;
    bool l = x < s.x0 + band, r = x >= s.x1 - band, t = y < s.y0 + band, b = y >= s.y1 - band;
    bool lc = x < s.x0 + corner, rc = x >= s.x1 - corner, tc = y < s.y0 + corner, bc = y >= s.y1 - corner;
    if ((t && lc) || (l && tc)) return HTTOPLEFT;
    if ((t && rc) || (r && tc)) return HTTOPRIGHT;
    if ((b && lc) || (l && bc)) return HTBOTTOMLEFT;
    if ((b && rc) || (r && bc)) return HTBOTTOMRIGHT;
    if (l) return HTLEFT;
    if (r) return HTRIGHT;
    if (t) return HTTOP;
    if (b) return HTBOTTOM;
    return HTCLIENT;
}

int GlassWindow::ButtonAt(float x, float y, bool* overPill) {
    Layout L;
    {
        std::lock_guard<std::mutex> lk(stateMu_);
        L = pubLayout_;
    }
    if (overPill) *overPill = L.pill.Inflate(4.0f * dp_).Contains(x, y);
    for (int i = 0; i < L.buttons; ++i) {
        float dx = x - L.btnX[i], dy = y - L.btnY;
        if (dx * dx + dy * dy <= (L.btnR * 1.2f) * (L.btnR * 1.2f)) return i;
    }
    return -1;
}

void GlassWindow::UpdateHover(float x, float y) {
    bool over = false;
    int b = ButtonAt(x, y, &over);
    overPill_ = over;
    if (b != hoverBtn_) {
        hoverBtn_ = b;
        std::lock_guard<std::mutex> lk(stateMu_);
        shared_.hoverBtn = b >= 0 ? b : tvHighlight_;
    }
    Wake();
}

void GlassWindow::UpdateControls() {
    if (!visible_ || debugHover_ >= 0) return;
    bool mouseWants = mouseInside_ &&
                      (NowSeconds() - lastMouseMove_ < kControlsIdleSeconds || overPill_ || pressedBtn_ >= 0);
    bool want = !dismissing_ && !tvHiding_ && (mouseWants || tvHighlight_ >= 0);
    if (want != controlsShown_) {
        controlsShown_ = want;
        {
            std::lock_guard<std::mutex> lk(stateMu_);
            shared_.controlsTarget = want ? 1.0f : 0.0f;
        }
        Wake();
    }
    bool hide = fullscreen_ && mouseInside_ && !want;
    if (hide != cursorHidden_) {
        cursorHidden_ = hide;
        POINT pt;
        GetCursorPos(&pt);
        if (WindowFromPoint(pt) == hwnd_) SetCursor(hide ? nullptr : LoadCursorW(nullptr, IDC_ARROW));
    }
}

void GlassWindow::ActivateButton(int b) {
    if (tv_) {
        TvButtonClicked(b);
        return;
    }
    switch (b) {
    case 0:
        LOGI("ui: close pressed");
        if (onUserClose) onUserClose();
        break;
    case 1: SetPinned(!pinned_); break;
    case 2:
        if (fullscreen_) ExitFullscreen();
        else EnterFullscreen();
        break;
    default: break;
    }
}

void GlassWindow::ShowContextMenu(POINT pt) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (pinned_ ? MF_CHECKED : 0), 1, L"Keep on top\tP");
    AppendMenuW(m, MF_STRING | (fullscreen_ ? MF_CHECKED : 0), 2, L"Full screen\tF");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 3, L"Disconnect");
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(m);
    if (cmd == 1) SetPinned(!pinned_);
    if (cmd == 2) ActivateButton(2);
    if (cmd == 3) ActivateButton(0);
}

// ---------------------------------------------------------------------------------------------
// Window procedure

LRESULT CALLBACK GlassWindow::StaticWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        reinterpret_cast<GlassWindow*>(cs->lpCreateParams)->hwnd_ = h;
    }
    auto* self = reinterpret_cast<GlassWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (self) return self->WndProc(m, w, l);
    return DefWindowProcW(h, m, w, l);
}

LRESULT GlassWindow::WndProc(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_NCCALCSIZE:
        if (w) return 0;
        break;
    case WM_NCHITTEST: return HitTest(GET_X_LPARAM(l), GET_Y_LPARAM(l));
    case WM_MOUSEACTIVATE:
        if (tv_) return MA_NOACTIVATE;  // TV Mode keeps the keyboard/remote focus
        break;
    case WM_NCACTIVATE: return DefWindowProcW(hwnd_, m, w, -1);
    case WM_NCPAINT: return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd_, &ps);
        EndPaint(hwnd_, &ps);
        Wake();
        return 0;
    }
    case WM_SIZING: {
        if (fullscreen_) break;
        RECT* r = reinterpret_cast<RECT*>(l);
        float M = Margin(), a = aspect_;
        float cw = float(r->right - r->left) - 2 * M, ch = float(r->bottom - r->top) - 2 * M;
        switch (w) {
        case WMSZ_LEFT:
        case WMSZ_RIGHT: ch = cw / a; break;
        case WMSZ_TOP:
        case WMSZ_BOTTOM: cw = ch * a; break;
        default:
            if (cw / a > ch) ch = cw / a;
            else cw = ch * a;
            break;
        }
        float minLong = 200.0f * dp_;
        if (std::max(cw, ch) < minLong) {
            if (a >= 1.0f) { cw = minLong; ch = cw / a; }
            else { ch = minLong; cw = ch * a; }
        }
        LONG W = LONG(std::lround(cw + 2 * M)), H = LONG(std::lround(ch + 2 * M));
        switch (w) {
        case WMSZ_LEFT: r->left = r->right - W; r->bottom = r->top + H; break;
        case WMSZ_RIGHT: r->right = r->left + W; r->bottom = r->top + H; break;
        case WMSZ_TOP: r->top = r->bottom - H; r->right = r->left + W; break;
        case WMSZ_BOTTOM: r->bottom = r->top + H; r->right = r->left + W; break;
        case WMSZ_TOPLEFT: r->left = r->right - W; r->top = r->bottom - H; break;
        case WMSZ_TOPRIGHT: r->right = r->left + W; r->top = r->bottom - H; break;
        case WMSZ_BOTTOMLEFT: r->left = r->right - W; r->bottom = r->top + H; break;
        default: r->right = r->left + W; r->bottom = r->top + H; break;
        }
        return TRUE;
    }
    case WM_WINDOWPOSCHANGING: {
        auto* wp = reinterpret_cast<WINDOWPOS*>(l);
        if (inSizeMove_ && !sizing_ && !(wp->flags & SWP_NOSIZE)) {
            // Moving: never let Aero Snap resize the glass.
            wp->cx = winRect_.right - winRect_.left;
            wp->cy = winRect_.bottom - winRect_.top;
        }
        break;
    }
    case WM_SIZE:
        if (w == SIZE_MINIMIZED) {
            std::lock_guard<std::mutex> lk(stateMu_);
            shared_.iconic = true;
            return 0;
        }
        {
            std::lock_guard<std::mutex> lk(stateMu_);
            shared_.iconic = false;
        }
        if (!internalMove_) OnExternalResize();
        return 0;
    case WM_MOVE:
        if (!internalMove_) {
            RECT r;
            GetWindowRect(hwnd_, &r);
            float dx = float(r.left - winRect_.left), dy = float(r.top - winRect_.top);
            shrinkTo_ = shrinkTo_.Offset(dx, dy);
            animTarget_ = animTarget_.Offset(dx, dy);
            winRect_ = r;
            if (inSizeMove_ && (dx != 0 || dy != 0)) movedInLoop_ = true;
        }
        return 0;
    case WM_ENTERSIZEMOVE:
        inSizeMove_ = true;
        movedInLoop_ = false;
        return 0;
    case WM_EXITSIZEMOVE:
        inSizeMove_ = false;
        if (sizing_) RememberSize();
        if (tv_ && sessionLive_ && (movedInLoop_ || sizing_)) {
            tvFree_ = true;  // placed by hand: TV Mode stops managing the corner/size
            if (onTvLayoutChanged) onTvLayoutChanged(true);
        }
        sizing_ = false;
        movedInLoop_ = false;
        return 0;
    case WM_SYSCOMMAND: {
        UINT cmd = UINT(w & 0xFFF0);
        if (cmd == SC_SIZE) {
            if (fullscreen_) return 0;
            sizing_ = true;
        } else if (cmd == SC_MOVE) {
            if (fullscreen_) return 0;
            sizing_ = false;
        } else if (cmd == SC_MAXIMIZE) {
            EnterFullscreen();
            return 0;
        } else if (cmd == SC_RESTORE && fullscreen_ && !IsIconic(hwnd_)) {
            ExitFullscreen();
            return 0;
        }
        break;
    }
    case WM_DPICHANGED: {
        dp_ = float(HIWORD(w)) / 96.0f;
        {
            std::lock_guard<std::mutex> lk(stateMu_);
            shared_.dp = dp_;
            shared_.labelDirty = true;
        }
        const RECT* r = reinterpret_cast<const RECT*>(l);
        if (!fullscreen_)
            SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_MOUSEMOVE: {
        float x = float(GET_X_LPARAM(l)), y = float(GET_Y_LPARAM(l));
        if (!mouseInside_) {
            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd_;
            TrackMouseEvent(&tme);
            mouseInside_ = true;
        }
        lastMouseMove_ = NowSeconds();
        if (dragArmed_ && (std::abs(LONG(x) - dragStart_.x) > GetSystemMetrics(SM_CXDRAG) ||
                           std::abs(LONG(y) - dragStart_.y) > GetSystemMetrics(SM_CYDRAG))) {
            dragArmed_ = false;
            ReleaseCapture();
            POINT pt;
            GetCursorPos(&pt);
            SendMessageW(hwnd_, WM_NCLBUTTONDOWN, HTCAPTION, MAKELPARAM(pt.x, pt.y));
            return 0;
        }
        UpdateHover(x, y);
        UpdateControls();
        return 0;
    }
    case WM_MOUSELEAVE:
        mouseInside_ = false;
        overPill_ = false;
        if (hoverBtn_ != -1) {
            hoverBtn_ = -1;
            std::lock_guard<std::mutex> lk(stateMu_);
            shared_.hoverBtn = tvHighlight_;
        }
        UpdateControls();
        Wake();
        return 0;
    case WM_LBUTTONDOWN: {
        float x = float(GET_X_LPARAM(l)), y = float(GET_Y_LPARAM(l));
        SetCapture(hwnd_);
        bool over = false;
        int b = ButtonAt(x, y, &over);
        if (b >= 0 && pubControls_ > 0.3f) {
            pressedBtn_ = b;
            std::lock_guard<std::mutex> lk(stateMu_);
            shared_.pressing = true;
        } else if (!fullscreen_) {
            dragArmed_ = true;
            dragStart_ = POINT{LONG(x), LONG(y)};
        }
        Wake();
        return 0;
    }
    case WM_LBUTTONUP: {
        float x = float(GET_X_LPARAM(l)), y = float(GET_Y_LPARAM(l));
        ReleaseCapture();
        dragArmed_ = false;
        if (pressedBtn_ >= 0) {
            int pb = pressedBtn_;
            pressedBtn_ = -1;
            {
                std::lock_guard<std::mutex> lk(stateMu_);
                shared_.pressing = false;
            }
            if (ButtonAt(x, y, nullptr) == pb) ActivateButton(pb);
        }
        Wake();
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        bool over = false;
        if (ButtonAt(float(GET_X_LPARAM(l)), float(GET_Y_LPARAM(l)), &over) < 0 && !over) {
            if (tv_) TvButtonClicked(kTvFull);
            else if (fullscreen_) ExitFullscreen();
            else EnterFullscreen();
        }
        return 0;
    }
    case WM_RBUTTONUP: {
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        ClientToScreen(hwnd_, &pt);
        ShowContextMenu(pt);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        if (fullscreen_ || !visible_) return 0;
        float steps = float(GET_WHEEL_DELTA_WPARAM(w)) / float(WHEEL_DELTA);
        float f = std::pow(1.10f, steps);
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        RectF cur = hasAnimTarget_ ? animTarget_ : CurrentSlabScreen();
        HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        RectF wa = WorkArea(mon).Inflate(-8.0f * dp_);
        float nw = cur.W() * f, nh = cur.H() * f;
        float minLong = 200.0f * dp_;
        if (std::max(nw, nh) < minLong) {
            float k = minLong / std::max(nw, nh);
            nw *= k;
            nh *= k;
        }
        float fit = std::min({1.0f, wa.W() / nw, wa.H() / nh});
        nw *= fit;
        nh *= fit;
        float ax = std::clamp((float(pt.x) - cur.x0) / std::max(cur.W(), 1.0f), 0.0f, 1.0f);
        float ay = std::clamp((float(pt.y) - cur.y0) / std::max(cur.H(), 1.0f), 0.0f, 1.0f);
        RectF r{float(pt.x) - ax * nw, float(pt.y) - ay * nh, float(pt.x) - ax * nw + nw, float(pt.y) - ay * nh + nh};
        AnimateSlabTo(ClampToWork(r, mon));
        RememberSize();
        if (tv_ && sessionLive_) {
            tvFree_ = true;
            if (onTvLayoutChanged) onTvLayoutChanged(true);
        }
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) {
            SetCursor(cursorHidden_ ? nullptr : LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_KEYDOWN:
        switch (w) {
        case VK_ESCAPE:
            if (fullscreen_) ExitFullscreen();
            return 0;
        case VK_F11:
        case 'F':
            if (fullscreen_) ExitFullscreen();
            else EnterFullscreen();
            return 0;
        case 'P':
        case 'T': SetPinned(!pinned_); return 0;
        default: break;
        }
        break;
    case WM_TIMER:
        if (w == kTimerIdle) UpdateControls();
        if (w == kTimerTvPress) {
            KillTimer(hwnd_, kTimerTvPress);
            std::lock_guard<std::mutex> lk(stateMu_);
            shared_.pressing = pressedBtn_ >= 0;
            Wake();
        }
        if (w == kTimerDebugFsIn) {
            KillTimer(hwnd_, kTimerDebugFsIn);
            EnterFullscreen();
        }
        if (w == kTimerDebugFsOut) {
            KillTimer(hwnd_, kTimerDebugFsOut);
            ExitFullscreen();
        }
        return 0;
    case WM_CLOSE:
        LOGI("ui: window closed by user");
        if (onUserClose) onUserClose();
        return 0;
    case kMsgSettled:
        if (pendingShrink_ && !inSizeMove_) {
            pendingShrink_ = false;
            hasAnimTarget_ = false;
            RECT t = shrinkTo_.ToRECT();
            if (!fullscreen_ && !SameRect(t, winRect_)) SetWindowRectSync(shrinkTo_);
        }
        return 0;
    case kMsgDismissed:
        if (dismissing_ || tvHiding_) {
            if (!dismissing_ && sessionLive_) tvHidden_ = true;  // TV "hidden": the session goes on
            ShowWindow(hwnd_, SW_HIDE);
            visible_ = false;
            dismissing_ = false;
            tvHiding_ = false;
            fullscreen_ = false;
            pendingShrink_ = false;
            hasAnimTarget_ = false;
            mouseInside_ = false;
            controlsShown_ = false;
            cursorHidden_ = false;
            KillTimer(hwnd_, kTimerIdle);
            std::lock_guard<std::mutex> lk(stateMu_);
            shared_.visible = false;
            shared_.fsTarget = 0.0f;
            shared_.controlsTarget = 0.0f;
            shared_.hoverBtn = -1;
            hoverBtn_ = -1;
            LOGI("ui: window hidden");
        }
        return 0;
    default: break;
    }
    return DefWindowProcW(hwnd_, m, w, l);
}
