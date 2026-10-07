// The Liquid Glass mirroring window: a borderless per-pixel-alpha DirectComposition window
// with its own render thread, spring animations and D3D11 video pipeline.
#pragma once

#include "common.h"
#include "media/frame_sink.h"

#include <d2d1_1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite.h>
#include <dxgi1_3.h>

class GlassWindow : public FrameSink {
public:
    struct Options {
        bool pinned = false;
        int longPortrait = 0;   // remembered slab long side (px) per orientation
        int longLandscape = 0;
    };
    // Placement requested by TV Mode (over the control pipe).
    struct TvLayout {
        enum Mode { Float = 0, Full = 1, Hidden = 2 };
        int mode = Float;
        int corner = 0;   // 0 bottom-right, 1 bottom-left, 2 top-right, 3 top-left
        int size = 0;     // 0 small, 1 medium, 2 large
        int margin = 48;  // physical px between the glass and the screen edge
    };
    // TV capsule buttons (left to right).
    enum TvButton { kTvFull = 0, kTvCorner = 1, kTvSize = 2, kTvStop = 3 };

    GlassWindow();
    ~GlassWindow() override;

    bool Create(HINSTANCE inst, HICON bigIcon, HICON smallIcon, const Options& opt);
    void Destroy();

    // Session lifecycle (UI thread).
    void BeginSession(uint64_t sid, const std::wstring& deviceName, const std::string& model);
    void EndSession(uint64_t sid);
    void SetVideoSize(uint64_t sid, int w, int h);
    uint64_t Session() const { return sid_; }
    bool SessionLive() const { return sessionLive_; }
    bool Visible() const { return visible_; }
    void BringToFront();
    Options CurrentOptions() const { return opt_; }

    // TV Mode control (UI thread). While TV-controlled the window floats topmost at a corner,
    // never takes focus, and its capsule offers full screen / move corner / size / stop.
    void SetTvControlled(bool on);
    bool TvControlled() const { return tv_; }
    void SetTvDefaults(const TvLayout& l) { tvDefaults_ = l; }
    void ApplyTvLayout(const TvLayout& l, bool byUser = false);  // current session
    void SetTvHighlight(int button);        // -1 hides the capsule
    void PressTvButton(int button);         // press animation only
    TvLayout CurrentTvLayout() const { return tvLayout_; }
    // "float" | "full" | "hidden" | "free" (moved or resized by hand) and the glass rect on screen.
    const char* TvModeName() const;
    RECT TvRect();

    std::function<void()> onUserClose;
    std::function<void()> onOptionsChanged;
    std::function<void(bool byUser)> onTvLayoutChanged;

    // FrameSink (decoder threads)
    ID3D11Device* GpuDevice() override { return dev_.Get(); }
    std::recursive_mutex& GpuLock() override { return gpuLock_; }
    void PresentHwFrame(ID3D11Texture2D* tex, unsigned index, int width, int height, const VideoColor& color) override;
    void PresentSwFrame(const uint8_t* const planes[3], const int strides[3], bool nv12, int width, int height,
                        const VideoColor& color) override;

    // Debug: renders the next frame and saves nothing; used by tests to force a frame.
    void Wake();

private:
    struct Spring {
        float x = 0, v = 0;
    };
    struct Layout {
        RectF slab, video, pill;
        float radius = 0, bezel = 0, uiScale = 1, btnR = 16, iconHalf = 7, stroke = 1.6f;
        int buttons = 3;
        float btnX[4] = {0, 0, 0, 0}, btnY = 0, spacing = 40;
    };
    struct Shared {
        bool visible = false, iconic = false;
        int winW = 0, winH = 0;
        float dp = 1;
        RectF slabTarget;
        bool snap = false, resetAnim = false;
        float appearTarget = 0, fsTarget = 0, controlsTarget = 0;
        int hoverBtn = -1;
        bool pressing = false, pinned = false;
        bool tv = false;
        int tvCorner = 0, tvSize = 0;
        float tvBoost = 1.0f;  // controls readable from the couch: 1080p-equivalent size on big screens
        float aspect = 0.4613f;
        uint64_t sessionStartSeq = 0;
        bool labelDirty = false;
        std::wstring labelName;
        bool settleRequest = false, dismissRequest = false;
    };
    struct Anim {
        Spring x0, y0, x1, y1, appear, fs, controls, dropX, dropA, press, reveal;
        double lastTime = 0, t0 = 0;
    };

    // Setup
    bool CreateDevice();
    bool CreateSwapChain();
    bool CreatePipeline();
    static LRESULT CALLBACK StaticWndProc(HWND h, UINT m, WPARAM w, LPARAM l);
    LRESULT WndProc(UINT m, WPARAM w, LPARAM l);

    // Rendering
    void RenderThread();
    void RenderLocked();  // requires renderMu_
    void EnsureSwapSize(int w, int h);
    void ConvertVideoLocked();
    void RenderLabelLocked(const std::wstring& name, float dp);
    static Layout ComputeLayout(const RectF& slab, float fs, float dp, float aspect, int buttons, float boost);
    void UpdateTvBoost();

    // Geometry (UI thread)
    float Margin() const { return 28.0f * dp_; }
    RectF CurrentSlabScreen();
    RectF WorkArea(HMONITOR mon) const;
    RectF ClampToWork(RectF r, HMONITOR mon) const;
    RectF InitialSlab(HMONITOR mon, float aspect) const;
    RectF SlabForAspect(const RectF& cur, float aspect) const;
    void SetWindowRectSync(const RectF& winScreen);
    void AnimateSlabTo(const RectF& targetScreen);
    void OnExternalResize();
    void EnterFullscreen(HMONITOR mon = nullptr);
    void ExitFullscreen();
    // TV placement
    HMONITOR TvMonitor(RectF* area) const;
    RectF TvFloatSlab(const TvLayout& l, float aspect) const;
    void ShowWindowForSession();
    void HideForTv();
    void TvButtonClicked(int b);
    void SetPinned(bool pinned);
    void RememberSize();
    LRESULT HitTest(int sx, int sy);
    int ButtonAt(float x, float y, bool* overPill);
    void UpdateHover(float x, float y);
    void UpdateControls();
    void ActivateButton(int b);
    void ShowContextMenu(POINT screenPt);

    // GPU
    ComPtr<ID3D11Device> dev_;
    ComPtr<ID3D11DeviceContext> ctx_;
    std::recursive_mutex gpuLock_;
    ComPtr<IDXGISwapChain2> swap_;
    HANDLE frameWait_ = nullptr;
    bool deviceLost_ = false;
    ComPtr<ID3D11RenderTargetView> rtv_;
    UINT bufW_ = 0, bufH_ = 0;
    ComPtr<IDCompositionDevice> dcomp_;
    ComPtr<IDCompositionTarget> dtarget_;
    ComPtr<IDCompositionVisual> dvisual_;
    ComPtr<ID3D11VertexShader> vsFull_;
    ComPtr<ID3D11PixelShader> psCompose_, psConvert_;
    ComPtr<ID3D11Buffer> cbCompose_, cbConvert_;
    ComPtr<ID3D11SamplerState> sampLinear_;

    // Video textures (written by decoder threads under gpuLock_)
    ComPtr<ID3D11Texture2D> nv12_;
    ComPtr<ID3D11ShaderResourceView> nv12Y_, nv12UV_;
    int nv12W_ = 0, nv12H_ = 0;
    ComPtr<ID3D11Texture2D> swY_, swU_, swV_, swUV_;
    ComPtr<ID3D11ShaderResourceView> swYSrv_, swUSrv_, swVSrv_, swUVSrv_;
    int srcW_ = 0, srcH_ = 0;
    int srcKind_ = 0;  // 0 none, 1 hw nv12, 2 sw planar, 3 sw nv12
    VideoColor srcColor_;
    std::atomic<uint64_t> frameSeq_{0};
    uint64_t convertedSeq_ = 0;
    ComPtr<ID3D11Texture2D> rgb_;
    ComPtr<ID3D11ShaderResourceView> rgbSrv_;
    ComPtr<ID3D11RenderTargetView> rgbRtv_;
    int rgbW_ = 0, rgbH_ = 0;

    // Label
    ComPtr<ID2D1Factory1> d2d_;
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<ID3D11Texture2D> label_;
    ComPtr<ID3D11ShaderResourceView> labelSrv_;
    int labelW_ = 0, labelH_ = 0;

    // Window / UI thread state
    HWND hwnd_ = nullptr;
    RECT winRect_{};
    float dp_ = 1.0f;
    bool visible_ = false, dismissing_ = false, fullscreen_ = false, internalMove_ = false;
    bool pinned_ = false;
    float aspect_ = 0.4613f;
    uint64_t sid_ = 0;
    RectF restoreSlab_;
    bool pendingShrink_ = false;
    RectF shrinkTo_;
    bool hasAnimTarget_ = false;
    RectF animTarget_;
    bool mouseInside_ = false, overPill_ = false, controlsShown_ = false, cursorHidden_ = false;
    double lastMouseMove_ = 0;
    int hoverBtn_ = -1, pressedBtn_ = -1;
    bool dragArmed_ = false;
    POINT dragStart_{};
    bool inSizeMove_ = false, sizing_ = false, movedInLoop_ = false;
    Options opt_;
    // TV Mode
    bool tv_ = false, sessionLive_ = false, tvHidden_ = false, tvHiding_ = false, tvFree_ = false;
    TvLayout tvDefaults_, tvLayout_;
    int tvHighlight_ = -1;
    // Test hooks (environment variables), used for automated screenshots.
    int debugHover_ = -1;
    float debugFsIn_ = 0, debugFsOut_ = 0;
    std::wstring snapDir_;              // AIRGLASS_DEBUG_SNAPSHOT: render hidden, save frames here
    std::vector<float> snapTimes_;      // seconds after session start
    size_t snapNext_ = 0;
    double sessionStart_ = 0;
    void SaveSnapshotLocked(int index);

    // Threads / sync
    std::mutex stateMu_;   // guards shared_, pub*
    std::mutex renderMu_;  // serialises frames (render thread vs synchronous UI renders); guards anim_
    Shared shared_;
    Anim anim_;
    RectF pubSlab_;
    Layout pubLayout_;
    float pubControls_ = 0;
    std::atomic<bool> needFrame_{false};
    std::atomic<bool> quit_{false};
    Event wake_;
    std::thread renderThread_;
};
