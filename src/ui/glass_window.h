// The Liquid Glass mirroring window: a borderless per-pixel-alpha DirectComposition window
// with its own render thread, spring animations and D3D11 video pipeline.
#pragma once

#include "common.h"
#include "media/frame_sink.h"
#include "media/recorder.h"

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
        bool obs = false;       // OBS mode (Pro)
        int index = 0;          // 0 = the first window, 1 = the second (Pro: two devices at once)
        bool startFull = false;  // [window] full=1: every session opens full screen (Esc still floats it in Pro)
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
    // beside: the other mirroring window's glass on screen; a window that opens now goes next to it, not on top.
    void BeginSession(uint64_t sid, const std::wstring& deviceName, const std::string& model,
                      const RECT* beside = nullptr);
    void EndSession(uint64_t sid);
    void SetVideoSize(uint64_t sid, int w, int h);
    uint64_t Session() const { return sid_; }
    bool SessionLive() const { return sessionLive_; }
    bool Visible() const { return visible_; }
    void BringToFront();
    Options CurrentOptions() const { return opt_; }

    // Free edition: every session opens full screen, and leaving it (Esc, F, double-click, the
    // capsule, TV float) calls onUpgradeRequested instead. Pro unlocks the window.
    void SetPro(bool pro);
    bool Pro() const { return pro_; }
    HWND Hwnd() const { return hwnd_; }

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
    // The screen mirroring is on, or would open on now (the PIN code card goes there too).
    HMONITOR SessionMonitor() const;

    std::function<void()> onUserClose;
    std::function<void()> onActivated;  // the window got the focus (the tools and the sound follow it)
    std::function<void()> onNextWindow;  // Tab: go to the other mirroring window (two devices at once)
    std::function<void()> onOptionsChanged;
    std::function<void(bool byUser)> onTvLayoutChanged;
    // A Pro feature was used in the free edition: kProWindow (leaving full screen), kProScreenshot, kProRecord,
    // kProObs (OBS mode), kProZoom (region zoom), kProDraw (drawing on the picture); kProAllowList (asking before
    // new devices connect) and kProPin (requiring a code to connect) come from the tray menu; kProTwoDevices (a
    // second device took over the session) from the app.
    enum ProFeature {
        kProWindow = 0, kProScreenshot = 1, kProRecord = 2, kProObs = 3, kProZoom = 4, kProDraw = 5, kProAllowList = 6,
        kProPin = 7, kProTwoDevices = 8
    };
    std::function<void(int feature)> onUpgradeRequested;

    // Pro: screenshot of the glass window (PNG with transparency, saved by the caller's path) and the
    // mirrored picture alone on the clipboard. Rendered on the next frame; onScreenshot reports back
    // on the UI thread.
    void RequestScreenshot(const std::wstring& path);
    std::function<void(bool ok, const std::wstring& path)> onScreenshot;
    std::function<void()> onScreenshotKey;  // S key (Pro): the caller picks the file and calls RequestScreenshot
    // Pro: MP4 recording of the mirrored stream (R key or tray menu -> onRecordToggle).
    Recorder& Rec() { return recorder_; }
    std::function<void()> onRecordToggle;
    // The picture inside the video frame changed size (black side bars detected or gone).
    std::function<void(uint64_t sid, int width, int height)> onContentSize;

    // Pro: OBS mode (O key, tray or window menu). An opaque window for OBS Window Capture: chroma-key green
    // (#00B140) round the picture, no glass, shadow or capsule, and the fixed title "AirGlass (OBS)".
    void ToggleObsMode();
    bool ObsMode() const { return obs_ && pro_; }
    // Pro: region zoom. Z (or the menus) then drag a rectangle, or Ctrl+drag, to zoom smoothly into that part of
    // the picture; Z again, Esc or a double-click zooms back out. Screenshots and OBS mode show the zoomed view.
    void ToggleRegionZoom();
    bool Zoomed() const { return zoomS_ < 0.999f; }
    // Pro: draw on the picture. D (or the menus) toggles draw mode, where drags draw instead of moving the window.
    // P pen, A arrow, H highlighter; 1-4 red, yellow, green, blue; Backspace undoes a stroke, C clears them all.
    // Strokes are kept in picture coordinates, so they stay on the picture when the window moves, resizes or
    // zooms; they stay after draw mode ends and clear on a new session or a rotation. Screenshots and OBS mode
    // show them; recordings copy the sender's stream, so they don't.
    void ToggleDrawMode();
    bool DrawMode() const { return drawMode_; }
    bool HasDrawing();
    void ClearDrawing();

    // FrameSink (decoder threads)
    ID3D11Device* GpuDevice() override { return dev_.Get(); }
    std::recursive_mutex& GpuLock() override { return gpuLock_; }
    void PresentHwFrame(ID3D11Texture2D* tex, unsigned index, int width, int height, const VideoColor& color) override;
    void PresentSwFrame(const uint8_t* const planes[3], const int strides[3], bool nv12, int width, int height,
                        const VideoColor& color) override;
    void OnEncodedVideo(const uint8_t* data, size_t n) override { recorder_.Feed(data, n); }
    void OnAudioStarted(int sampleRate) override { recorder_.AudioStarted(sampleRate); }
    void OnAudioStopped() override { recorder_.AudioStopped(); }
    void OnDecodedAudio(const float* pcm, size_t frames) override { recorder_.FeedAudio(pcm, frames); }

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
        bool obs = false;  // OBS mode in effect (Pro)
        int tvCorner = 0, tvSize = 0;
        float tvBoost = 1.0f;  // controls readable from the couch: 1080p-equivalent size on big screens
        float aspect = 0.4613f;
        uint64_t sessionStartSeq = 0;
        bool labelDirty = false;
        std::wstring labelName;
        bool settleRequest = false, dismissRequest = false;
        float zoomCx = 0.5f, zoomCy = 0.5f, zoomS = 1.0f;  // region zoom: centre and size, in picture uv
        bool selOn = false;
        RectF sel;  // region-zoom rectangle being dragged (window px)
        bool drawHint = false;  // draw mode: show "Drawing (D to exit)"
        int drawTool = 0, drawColor = 0;
    };
    struct Anim {
        Spring x0, y0, x1, y1, appear, fs, controls, dropX, dropA, press, reveal;
        Spring zoomX{0.5f, 0}, zoomY{0.5f, 0}, zoomS{1.0f, 0};
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
    void DrawConvertLocked(ID3D11RenderTargetView* rt, int w, int h, float u0, float uScale);
    void ProbePillarboxLocked(int w, int h);
    void ApplyAspect(float a);
    void RenderLabelLocked(const std::wstring& name, float dp);
    static Layout ComputeLayout(const RectF& slab, float fs, float dp, float aspect, int buttons, float boost, bool obs);
    void ApplyObsMode();
    // Region zoom (UI thread)
    void SetZoom(float cx, float cy, float s);
    void ResetZoom();
    void ZoomToSelection(const RectF& sel);
    void SetSelection(const RectF* sel);  // nullptr hides it
    RectF SelectionRect(POINT a, POINT b);
    // Drawing (UI thread, except RenderInkLocked)
    enum DrawTool { kPen = 0, kArrow = 1, kHighlighter = 2 };
    struct Stroke {
        int tool = kPen, color = 0;
        float width = 0;                  // picture units (the picture is 1 high)
        std::vector<D2D1_POINT_2F> pts;   // picture units: x 0..width/height, y 0..1
        bool open = true;                 // still being drawn
        ComPtr<ID2D1PathGeometry> geo;    // built by the render thread once the stroke is finished
    };
    void SetDrawMode(bool on);
    void SetDrawTool(int tool);
    void SetDrawColor(int color);
    void PublishDrawHint();
    bool PictureFromClient(float x, float y, D2D1_POINT_2F* pt, float* pxPerUnit);
    void BeginStroke(float x, float y);
    void ExtendStroke(float x, float y);
    void EndStroke();
    void UndoStroke();
    HCURSOR DrawCursor();
    ComPtr<ID2D1PathGeometry> StrokeGeometry(const Stroke& s);
    // Draws the strokes (and the draw-mode hint) into ink_ for this frame's layout; false if there is nothing.
    bool RenderInkLocked(const Layout& L, const float zoom[4], const Shared& s, bool hint);
    void UpdateTvBoost();

    // Geometry (UI thread)
    float Margin() const { return 28.0f * dp_; }
    RectF CurrentSlabScreen();
    RectF WorkArea(HMONITOR mon) const;
    RectF ClampToWork(RectF r, HMONITOR mon) const;
    RectF InitialSlab(HMONITOR mon, float aspect) const;
    RectF SlabForAspect(const RectF& cur, float aspect) const;
    RectF BesideSlab(const RectF& slab, const RectF& other, HMONITOR mon) const;
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
    // Pillarbox crop (render thread). iPhones mirroring in portrait to a landscape display send
    // landscape frames with black bars; a 3-row probe of each frame finds them and the convert pass
    // crops them off, so the window takes the shape of the picture.
    ComPtr<ID3D11Texture2D> probe_, probeStage_;
    ComPtr<ID3D11RenderTargetView> probeRtv_;
    int probeW_ = 0;
    int cropSrcW_ = 0, cropSrcH_ = 0, cropX_ = 0, cropPending_ = -1;
    double lastProbe_ = 0;
    std::atomic<double> cropConfirmAt_{0};
    std::atomic<bool> forceConvert_{false};
    int rgbW_ = 0, rgbH_ = 0;

    // Label
    ComPtr<ID2D1Factory1> d2d_;
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<ID3D11Texture2D> label_;
    ComPtr<ID3D11ShaderResourceView> labelSrv_;
    int labelW_ = 0, labelH_ = 0;
    // Drawing layer (render thread): window-sized, sampled by the compose pass under the picture's controls.
    ComPtr<ID3D11Texture2D> ink_;
    ComPtr<ID3D11ShaderResourceView> inkSrv_;
    ComPtr<ID2D1RenderTarget> inkRt_;
    ComPtr<ID2D1SolidColorBrush> inkBrush_;
    ComPtr<ID2D1StrokeStyle> inkStyle_;
    ComPtr<IDWriteTextFormat> hintFormat_;
    int inkW_ = 0, inkH_ = 0;
    float hintDp_ = 0;  // hintFormat_ was made for this dp

    // Window / UI thread state
    HWND hwnd_ = nullptr;
    RECT winRect_{};
    float dp_ = 1.0f;
    bool visible_ = false, dismissing_ = false, fullscreen_ = false, internalMove_ = false;
    bool pinned_ = false;
    float aspect_ = 0.4613f;
    int vidW_ = 0, vidH_ = 0, contentW_ = 0;  // decoded frame size and the picture width inside it
    uint64_t sid_ = 0;
    bool besideValid_ = false;
    RectF beside_;  // the other window's glass when this session opened (screen px)
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
    bool pro_ = true;
    bool obs_ = false;  // OBS mode chosen (in effect only with Pro)
    bool zoomArmed_ = false, zoomSelecting_ = false;  // Z pressed / dragging the zoom rectangle
    POINT zoomFrom_{};
    float zoomCx_ = 0.5f, zoomCy_ = 0.5f, zoomS_ = 1.0f;  // target region (picture uv), 1 = the whole picture
    bool drawMode_ = false, drawing_ = false;  // draw mode on / a stroke is being dragged
    int drawTool_ = kPen, drawColor_ = 0;
    POINT lastInk_{};
    HCURSOR drawCursors_[4] = {};
    std::mutex drawMu_;            // guards strokes_ (UI thread edits, render thread draws)
    std::vector<Stroke> strokes_;
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
    void SaveScreenshotLocked(const Layout& L);
    std::atomic<bool> shotReq_{false};
    std::wstring shotPath_;  // guarded by stateMu_
    Recorder recorder_;

    // Threads / sync
    std::mutex stateMu_;   // guards shared_, pub*
    std::mutex renderMu_;  // serialises frames (render thread vs synchronous UI renders); guards anim_
    Shared shared_;
    Anim anim_;
    RectF pubSlab_;
    Layout pubLayout_;
    float pubControls_ = 0;
    float pubZoom_[4] = {0, 0, 1, 1};  // the part of the picture on screen this frame (uv)
    std::atomic<bool> needFrame_{false};
    std::atomic<bool> quit_{false};
    Event wake_;
    std::thread renderThread_;
};
