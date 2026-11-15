// AirGlass - AirPlay screen mirroring receiver for Windows with a Liquid Glass popup window.
#include "common.h"

#include <shellapi.h>
#include <shlobj.h>

#include <chrono>
#include <condition_variable>
#include <deque>

#include "config.h"
#include "control_pipe.h"
#include "license.h"
#include "crypto/crypto.h"
#include "media/audio_output.h"
#include "net/airplay_server.h"
#include "net/bplist.h"
#include "net/mdns.h"
#include "net/netutil.h"
#include "ui/allow.h"
#include "ui/glass_window.h"
#include "ui/icon.h"
#include "ui/pin_card.h"
#include "ui/rename.h"
#include "ui/welcome.h"

namespace {

constexpr UINT kMsgTray = WM_APP + 1;
constexpr UINT kMsgMirrorStart = WM_APP + 2;
constexpr UINT kMsgMirrorStop = WM_APP + 3;
constexpr UINT kMsgVideoSize = WM_APP + 4;
constexpr UINT kMsgShowExisting = WM_APP + 5;
constexpr UINT kMsgPipeLine = WM_APP + 6;     // lParam: std::string* (one JSON command)
constexpr UINT kMsgPipeClients = WM_APP + 7;  // wParam: connected client count
constexpr UINT kMsgNowPlaying = WM_APP + 8;
constexpr UINT kMsgMediaDone = WM_APP + 9;    // lParam: MediaResult*
constexpr UINT kMsgUpgrade = WM_APP + 10;     // free edition: a Pro feature was used (wParam: GlassWindow::ProFeature)
constexpr UINT kMsgLicenseRevoked = WM_APP + 11;
constexpr UINT kMsgTrialEnded = WM_APP + 12;
constexpr UINT kMsgRecordStopped = WM_APP + 13;  // lParam: std::wstring* (finished file), recorder thread
constexpr UINT kMsgRenamed = WM_APP + 14;        // lParam: RenameResult* (checked on the network)
constexpr UINT kMsgAskDevice = WM_APP + 15;      // lParam: std::shared_ptr<AdmitRequest>* (connection thread)
constexpr UINT kMsgPinCode = WM_APP + 16;        // lParam: std::string* (code to show, "" to hide; connection thread)
constexpr UINT kMsgPinPaired = WM_APP + 17;      // lParam: PairedDevice* (connection thread)
constexpr UINT_PTR kTimerNowPlaying = 1;
constexpr UINT_PTR kTimerTvGrace = 2;
constexpr UINT_PTR kTimerNpDebounce = 3;
constexpr UINT_PTR kTimerPromoteTray = 4;  // first run: retry until Explorer has registered our tray icon
constexpr UINT kMenuAutostart = 101, kMenuLogs = 102, kMenuQuit = 103, kMenuShow = 104, kMenuPro = 105;
constexpr UINT kMenuShot = 106, kMenuRecord = 107, kMenuMediaFolder = 108, kMenuRename = 109;
constexpr UINT kMenuObs = 110, kMenuZoom = 111, kMenuDraw = 112, kMenuAskDevices = 113, kMenuForgetDevices = 114;
constexpr UINT kMenuRequirePin = 115, kMenuForgetPaired = 116;
constexpr int kAskSeconds = 30;  // allow-list: a device nobody answers for is turned away
constexpr const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

struct StartEvent {
    uint64_t sid;
    std::string name, model;
    int window;
};
struct MediaJob {
    std::string action, rid;
    double value = 0;
};
struct MediaResult {
    std::string rid, error;
};
struct PairedDevice {
    std::string key, name;  // Ed25519 key (hex), and the device id or name
};
struct RenameResult {
    std::string wanted, name;  // name = wanted, or wanted + " PC" if another device already uses it
};
// Allow-list: a connection waiting (on its own thread) for the user's answer about a new device.
struct AdmitRequest {
    std::string id, name;
    double deadline = 0;  // NowSeconds
    std::mutex mu;
    std::condition_variable cv;
    int answer = -1;  // ui::AllowAnswer once settled (guarded by mu)
};

const char* kCornerNames[4] = {"br", "bl", "tr", "tl"};
const char* kSizeNames[3] = {"s", "m", "l"};
const char* kButtonNames[4] = {"full", "corner", "size", "stop"};

int IndexOf(const char* const* names, int n, const std::string& v, int fallback) {
    for (int i = 0; i < n; ++i)
        if (v == names[i]) return i;
    return fallback;
}

std::string Str(const JsonObject& o, const char* k) {
    auto it = o.find(k);
    return it != o.end() && it->second.type == JsonValue::Type::String ? it->second.s : std::string();
}

GlassWindow::TvLayout ParseLayout(const JsonObject& o, GlassWindow::TvLayout l) {
    std::string mode = Str(o, "mode");
    if (mode == "float") l.mode = GlassWindow::TvLayout::Float;
    else if (mode == "full") l.mode = GlassWindow::TvLayout::Full;
    else if (mode == "hidden") l.mode = GlassWindow::TvLayout::Hidden;
    l.corner = IndexOf(kCornerNames, 4, Str(o, "corner"), l.corner);
    l.size = IndexOf(kSizeNames, 3, Str(o, "size"), l.size);
    auto m = o.find("margin");
    if (m != o.end() && m->second.type == JsonValue::Type::Number) l.margin = std::clamp(int(m->second.n), 0, 400);
    return l;
}

// Receiver names must be unique on the network (mDNS), e.g. not a speaker group with the PC's name.
std::string UniqueReceiverName(const std::string& base, const std::vector<std::string>& taken) {
    auto used = [&](const std::string& n) {
        for (auto& t : taken)
            if (_stricmp(t.c_str(), n.c_str()) == 0) return true;
        return false;
    };
    if (!used(base)) return base;
    std::string candidate = base + " PC";
    for (int i = 2; used(candidate) && i < 20; ++i) candidate = base + " PC " + std::to_string(i);
    return candidate;
}

const char* LayoutModeName(int mode) { return mode == 1 ? "full" : mode == 2 ? "hidden" : "float"; }

// The music session as JSON (without the "event" key when nested in a state event).
std::string NowPlayingJson(const NowPlaying& np, bool asEvent) {
    JsonWriter w;
    if (asEvent) w.Str("event", "nowplaying");
    w.Int("session", (long long)np.session).Bool("active", np.active);
    if (np.active) {
        w.Str("device", np.device)
            .Str("title", np.title)
            .Str("artist", np.artist)
            .Str("album", np.album)
            .Int("durationMs", np.durationMs)
            .Int("positionMs", np.positionMs)
            .Int("rate", np.playing ? 1 : 0)
            .Int("artworkSeq", np.artworkSeq)
            .Str("artworkType", np.artworkSeq ? np.artworkType : std::string())
            .Str("artworkPath", np.artworkSeq ? WideToUtf8(np.artworkPath) : std::string())
            .Num("volume", np.volume)
            .Bool("controls", np.controls)
            .Str("codec", AudioDecoder::Name(np.format.ct))
            .Int("sampleRate", np.format.sampleRate)
            .Int("bitDepth", np.format.bits)
            .Bool("lossless", np.format.Lossless());
    }
    return w.Done();
}

std::wstring ExePath() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return buf;
}

bool AutostartEnabled() {
    wchar_t buf[1024];
    DWORD size = sizeof(buf);
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, L"AirGlass", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS;
}

void SetAutostart(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        std::wstring cmd = L"\"" + ExePath() + L"\" --background";
        RegSetValueExW(k, L"AirGlass", 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
                       DWORD((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, L"AirGlass");
    }
    RegCloseKey(k);
}

// Windows 11 parks new tray icons in the hidden overflow. On first run, mark ours as shown next to
// the clock (the per-icon setting Explorer keeps under NotifyIconSettings). Returns false while
// Explorer has not created the entry yet. ExecutablePath may start with a known-folder GUID instead
// of the folder path, so the match is on the file name plus whichever form the path takes.
bool PromoteTrayIcon() {
    const std::wstring exe = ExePath();
    wchar_t local[MAX_PATH] = L"";
    GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    std::wstring asGuid = exe;
    if (*local && _wcsnicmp(exe.c_str(), local, wcslen(local)) == 0)
        asGuid = L"{F1B32785-6FBA-4FCF-9D55-7B8E7F157091}" + exe.substr(wcslen(local));
    HKEY root;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\NotifyIconSettings", 0, KEY_READ | KEY_SET_VALUE, &root) != ERROR_SUCCESS)
        return false;
    bool found = false;
    wchar_t sub[256];
    for (DWORD i = 0; !found; i++) {
        DWORD n = 256;
        if (RegEnumKeyExW(root, i, sub, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        wchar_t path[1024];
        DWORD size = sizeof(path);
        if (RegGetValueW(root, sub, L"ExecutablePath", RRF_RT_REG_SZ, nullptr, path, &size) != ERROR_SUCCESS) continue;
        if (_wcsicmp(path, exe.c_str()) != 0 && _wcsicmp(path, asGuid.c_str()) != 0) continue;
        DWORD one = 1;
        found = RegSetKeyValueW(root, sub, L"IsPromoted", REG_DWORD, &one, sizeof(one)) == ERROR_SUCCESS;
    }
    RegCloseKey(root);
    return found;
}

class App {
public:
    int Run(HINSTANCE inst, bool background, bool loopback);

private:
    static LRESULT CALLBACK StaticProc(HWND h, UINT m, WPARAM w, LPARAM l);
    LRESULT Proc(HWND h, UINT m, WPARAM w, LPARAM l);
    void AddTrayIcon();
    void UpdateTrayTip();
    void ShowTrayMenu();
    void Balloon(const std::wstring& title, const std::wstring& text);
    ReceiverInfo BuildIdentity();
    bool StartMdns();
    // "Rename this PC…" (UI thread): the dialog, then a network check off the UI thread, then ApplyName.
    void RenameReceiver();
    void ApplyName(const std::string& wanted, const std::string& name);
    // Allow-list. Admit runs on a connection thread; AskNextDevice shows the queued prompts (UI thread).
    bool Admit(const std::string& id, const std::string& name, const std::function<bool()>& gone);
    void AskNextDevice();
    // PIN pairing (UI thread): the setting, applied to the server and re-announced over mDNS.
    void ApplyPinRequired(bool on);

    // Mirroring windows (UI thread). Window 0 always exists; Pro adds window 1, so two devices can mirror at once.
    bool CreateGlass(int i);
    GlassWindow* Win(int i) { return i == 0 ? &glass_ : i == 1 ? glass2_.get() : nullptr; }
    int WindowOf(uint64_t sid);  // the window showing this live session, or -1
    int LiveCount();
    // The window the tray's tools and the sound follow: the one focused last, else the newest session.
    GlassWindow& Active();
    void FocusWindow(int i);
    void SetPro(bool on);      // every window, and how many sessions the server allows at once
    void UpdateMirrorSinks();
    void SaveRecordingAtEnd(GlassWindow& g, const wchar_t* why);
    void OnSessionSize(uint64_t sid, int w, int h);

    // TV Mode control pipe (UI thread).
    void OnPipeCommand(const std::string& line);
    void Emit(const std::string& json) { pipe_.Broadcast(json); }
    void EmitState();
    void EmitStopped(uint64_t sid, const char* reason);
    void EmitLayout(int window, bool byUser);
    GlassWindow& TvGlass();  // the window TV Mode drives (the most recent session)
    uint64_t TvSid() const { return tvWin_ >= 0 ? live_[tvWin_].sid : 0; }
    void EmitNowPlaying();
    void MediaLoop();
    // AirGlass Pro (UI thread).
    void ShowUpgrade(const std::wstring& reason);
    void CheckLicenseInBackground();
    bool Licensed() const { return !cfg_.licenseKey.empty() && !cfg_.licenseInstance.empty(); }
    // Days left in the free Pro trial: license::kTrialDays before the first session, 0 once it's over.
    int TrialDaysLeft() const;
    // Pro tools (UI thread): files go to Pictures\AirGlass and Videos\AirGlass.
    void TakeScreenshot(GlassWindow& g);
    void ToggleRecording(GlassWindow& g);
    std::wstring MediaFile(REFKNOWNFOLDERID folder, const wchar_t* ext, const wchar_t* tag = L"");

    HINSTANCE inst_ = nullptr;
    HWND msgWnd_ = nullptr;
    UINT taskbarCreated_ = 0;
    HICON iconBig_ = nullptr, iconSmall_ = nullptr;
    Config cfg_;
    static constexpr int kWindows = 2;
    GlassWindow glass_;                    // the first mirroring window
    std::unique_ptr<GlassWindow> glass2_;  // Pro: the second, made when Pro is on
    int active_ = 0;
    bool twoDevicesAsked_ = false;  // free edition: the Pro dialog about two devices was offered (once per run)
    AudioOutput audio_;
    AirPlayServer server_;
    MdnsResponder mdns_;
    ControlPipe pipe_;
    std::string name_;
    ReceiverInfo ri_;  // as advertised (mDNS) and served (/info)
    std::thread renameThread_;
    bool renaming_ = false;  // the dialog is open or the network check is running
    bool loopback_ = false, upgradeOpen_ = false;
    int promoteTries_ = 0;
    // Allow-list: askOn_ mirrors cfg_.askNewDevices for the connection threads; devMu_ guards cfg_.devices,
    // which only the UI thread changes.
    std::atomic<bool> askOn_{false};
    std::mutex devMu_;
    std::deque<std::shared_ptr<AdmitRequest>> askQueue_;
    bool asking_ = false;  // a prompt is open
    ui::PinCard pinCard_;

    // Mirroring sessions as reported to TV Mode, per window. TV Mode follows the most recent one (tvWin_); when it
    // ends while the other still runs, TV Mode follows that one (a fresh "state" event says so).
    struct Announced {
        uint64_t sid = 0;
        std::string device, model;
        int w = 0, h = 0;
    };
    Announced live_[kWindows];
    int tvWin_ = -1;
    GlassWindow::TvLayout tvDefaults_;
    int pipeClients_ = 0;
    // Music session.
    std::atomic<bool> npPending_{false};
    uint64_t npSid_ = 0;
    // Remote-control commands run off the UI thread (they wait for the sender).
    std::thread mediaThread_;
    std::mutex mediaMu_;
    std::condition_variable mediaCv_;
    std::deque<MediaJob> mediaQ_;
    bool mediaStop_ = false;
};

App* g_app = nullptr;

ReceiverInfo App::BuildIdentity() {
    ReceiverInfo ri;
    ri.name = cfg_.name;
    std::memcpy(ri.mac, cfg_.mac, 6);
    crypto::Ed25519KeypairFromSeed(cfg_.seed, ri.edPub, ri.edPriv);
    ri.pi = cfg_.pi;
    ri.displayUuid = cfg_.displayUuid;
    ri.features = 0x5A7FFEE6ull;  // mirroring + audio + legacy pairing; no URL/HLS video

    // Advertise the primary monitor's shape, capped at 1080p. An iPhone asked for 4K mirrors
    // landscape at ~28 fps (8.3 MP per frame), while ~2 MP frames (1080p, or 4K-tall portrait)
    // run at 55-60 fps. Full screen upscales with the bicubic video filter. A bigger display can
    // still be forced with width/height under [video] in config.ini.
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    int w = 1920, h = 1080, hz = 60;
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm)) {
        w = int(dm.dmPelsWidth);
        h = int(dm.dmPelsHeight);
        if (dm.dmDisplayFrequency > 1) hz = int(dm.dmDisplayFrequency);
    }
    if (w < h) std::swap(w, h);
    float scale = std::min({1.0f, 1920.0f / float(w), 1080.0f / float(h)});
    w = int(float(w) * scale) & ~1;
    h = int(float(h) * scale) & ~1;
    if (h < 1080) {
        w = 1920;
        h = 1080;
    }
    ri.width = cfg_.displayW > 0 ? cfg_.displayW : w;
    ri.height = cfg_.displayH > 0 ? cfg_.displayH : h;
    ri.refresh = cfg_.displayHz > 0 ? cfg_.displayHz : std::clamp(hz, 60, 120);
    ri.maxFps = cfg_.maxFps > 0 ? cfg_.maxFps : 120;
    return ri;
}

void App::AddTrayIcon() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = msgWnd_;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
    nid.uCallbackMessage = kMsgTray;
    nid.hIcon = iconSmall_;
    std::wstring tip = L"AirGlass — Screen Mirroring as “" + Utf8ToWide(name_) + L"”";
    wcsncpy(nid.szTip, tip.c_str(), 127);
    Shell_NotifyIconW(NIM_ADD, &nid);
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

void App::UpdateTrayTip() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = msgWnd_;
    nid.uID = 1;
    nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    std::wstring tip = L"AirGlass — Screen Mirroring as “" + Utf8ToWide(name_) + L"”";
    wcsncpy(nid.szTip, tip.c_str(), 127);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

bool App::StartMdns() {
    std::vector<MdnsResponder::Service> services;
    services.push_back({ri_.name, "_airplay._tcp", server_.Port(), ri_.AirPlayTxt()});
    services.push_back({ri_.MacHex() + "@" + ri_.name, "_raop._tcp", server_.Port(), ri_.RaopTxt()});
    std::string host;
    for (char c : DefaultReceiverName()) host += (isalnum(static_cast<unsigned char>(c)) || c == '-') ? c : '-';
    return mdns_.Start(host + "-AirGlass", services);  // a restart says goodbye for the old name first
}

void App::RenameReceiver() {
    // The tray still reaches msgWnd_ while a dialog is open, so this guards against a second one.
    if (renaming_ || cfg_.nameManaged) return;
    renaming_ = true;
    std::wstring name = Utf8ToWide(cfg_.name);
    HWND owner = Active().Visible() ? Active().Hwnd() : msgWnd_;
    std::string wanted;
    if (ui::ShowRenameDialog(owner, iconBig_, &name)) wanted = SanitizeReceiverName(WideToUtf8(name));
    if (wanted.empty() || wanted == cfg_.name) {
        renaming_ = false;
        return;
    }
    if (loopback_) {
        ApplyName(wanted, wanted);
        return;
    }
    // Browsing the network blocks for about a second: do it off the UI thread.
    if (renameThread_.joinable()) renameThread_.join();
    renameThread_ = std::thread([wanted, own = cfg_.name, target = msgWnd_] {
        std::vector<std::string> taken;
        for (auto& t : MdnsResponder::BrowseReceiverNames(1.2))
            if (_stricmp(t.c_str(), own.c_str()) != 0) taken.push_back(t);  // our own answer, under the old name
        auto* r = new RenameResult{wanted, UniqueReceiverName(wanted, taken)};
        PostMessageW(target, kMsgRenamed, 0, reinterpret_cast<LPARAM>(r));
    });
}

void App::ApplyName(const std::string& wanted, const std::string& name) {
    renaming_ = false;
    if (name == cfg_.name) return;
    LOGI("receiver: renamed \"%s\" -> \"%s\"", cfg_.name.c_str(), name.c_str());
    cfg_.name = name;
    cfg_.Save();
    name_ = loopback_ ? name + " (loopback test)" : name;
    ri_.name = name;
    server_.SetName(name);
    if (!loopback_ && !StartMdns()) LOGE("receiver: mDNS restart failed after renaming");
    UpdateTrayTip();
    EmitState();
    std::wstring text = L"This PC now appears as “" + Utf8ToWide(name) + L"” in Screen Mirroring.";
    if (name != wanted) text = L"Another device here is already called “" + Utf8ToWide(wanted) + L"”. " + text;
    Balloon(L"Renamed", text);
}

// Known devices are answered at once. A new one is asked about on the UI thread while this connection waits, and
// turned away when nobody answers within kAskSeconds or the sender gives up.
bool App::Admit(const std::string& id, const std::string& name, const std::function<bool()>& gone) {
    if (loopback_ || !askOn_) return true;  // loopback tests never prompt
    {
        std::lock_guard<std::mutex> lk(devMu_);
        auto it = cfg_.devices.find(id);
        if (it != cfg_.devices.end()) {
            LOGI("devices: \"%s\" (%s) is %s", name.c_str(), id.c_str(), it->second.allowed ? "allowed" : "blocked");
            return it->second.allowed;
        }
    }
    auto req = std::make_shared<AdmitRequest>();
    req->id = id;
    req->name = name;
    req->deadline = NowSeconds() + kAskSeconds;
    LOGI("devices: asking whether \"%s\" (%s) may connect", name.c_str(), id.c_str());
    PostMessageW(msgWnd_, kMsgAskDevice, 0, reinterpret_cast<LPARAM>(new std::shared_ptr<AdmitRequest>(req)));
    std::unique_lock<std::mutex> lk(req->mu);
    while (req->answer < 0) {
        req->cv.wait_for(lk, std::chrono::milliseconds(250));
        if (req->answer >= 0) break;
        if (NowSeconds() >= req->deadline) {
            req->answer = ui::kAllowDeny;
            LOGI("devices: no answer about \"%s\" within %d s; turned away", name.c_str(), kAskSeconds);
            break;
        }
        lk.unlock();
        bool g = gone();
        lk.lock();
        if (g && req->answer < 0) {
            req->answer = ui::kAllowDeny;
            LOGI("devices: \"%s\" stopped waiting", name.c_str());
        }
    }
    return req->answer == ui::kAllowOnce || req->answer == ui::kAllowAlways;
}

void App::AskNextDevice() {
    if (asking_) return;  // the prompt that is open picks up the rest when it closes
    asking_ = true;
    auto settle = [](const std::shared_ptr<AdmitRequest>& r, int a) {
        std::lock_guard<std::mutex> lk(r->mu);
        if (r->answer < 0) r->answer = a;
        r->cv.notify_all();
    };
    while (!askQueue_.empty()) {
        std::shared_ptr<AdmitRequest> req = askQueue_.front();
        askQueue_.pop_front();
        auto settled = [req] {
            std::lock_guard<std::mutex> lk(req->mu);
            return req->answer >= 0;
        };
        if (settled()) continue;  // timed out, or the sender gave up, before its turn
        int a;
        auto known = cfg_.devices.find(req->id);  // answered for an earlier attempt meanwhile
        bool wasKnown = known != cfg_.devices.end();
        if (wasKnown) a = known->second.allowed ? ui::kAllowAlways : ui::kAllowBlock;
        else if (!askOn_) a = ui::kAllowOnce;  // turned off while it waited
        else a = ui::ShowAllowDialog(nullptr, iconBig_, Utf8ToWide(req->name), req->deadline, settled);
        if (!wasKnown && (a == ui::kAllowAlways || a == ui::kAllowBlock)) {
            {
                std::lock_guard<std::mutex> lk(devMu_);
                cfg_.devices[req->id] = Config::KnownDevice{a == ui::kAllowAlways, req->name};
            }
            cfg_.Save();
        }
        static const char* const kAnswers[] = {"turned away", "allowed once", "always allowed", "blocked"};
        LOGI("devices: \"%s\" (%s) %s", req->name.c_str(), req->id.c_str(), kAnswers[std::clamp(a, 0, 3)]);
        settle(req, a);
        for (auto& other : askQueue_)  // the same device again (senders retry): same answer, no second prompt
            if (other->id == req->id) settle(other, a);
    }
    asking_ = false;
}

void App::ApplyPinRequired(bool on) {
    server_.SetPinRequired(on);
    ri_.pin = on;
    LOGI("pin: require a code to connect: %s", on ? "on" : "off");
    if (!loopback_ && !StartMdns()) LOGE("receiver: mDNS restart failed after changing the code setting");
}

void App::Balloon(const std::wstring& title, const std::wstring& text) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = msgWnd_;
    nid.uID = 1;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
    nid.hBalloonIcon = iconBig_;
    wcsncpy(nid.szInfoTitle, title.c_str(), 63);
    wcsncpy(nid.szInfo, text.c_str(), 255);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void App::ShowTrayMenu() {
    HMENU m = CreatePopupMenu();
    std::wstring head = L"Receiving as “" + Utf8ToWide(name_) + L"”";
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, head.c_str());
    if (cfg_.nameManaged) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"Name set by your organisation");
    else AppendMenuW(m, MF_STRING | (renaming_ ? MF_GRAYED : 0), kMenuRename, L"&Rename this PC…");
    if (pipeClients_ > 0) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"Connected to TV Mode");
    GlassWindow& g = Active();
    int live = LiveCount();
    if (g.Visible()) AppendMenuW(m, MF_STRING, kMenuShow, live > 1 ? L"Show mirroring windows" : L"Show mirroring window");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    if (g.Visible() && g.SessionLive()) {
        bool pro = glass_.Pro();
        int wi = WindowOf(g.Session());
        if (live > 1 && wi >= 0) {  // two devices: say which window the tools act on (the one used last)
            std::wstring which = L"Tools for “" + Utf8ToWide(live_[wi].device) + L"”";
            AppendMenuW(m, MF_STRING | MF_GRAYED, 0, which.c_str());
        }
        AppendMenuW(m, MF_STRING, kMenuShot, pro ? L"Save screenshot\tS" : L"Save screenshot (Pro)…");
        std::wstring rec = L"Start recording\tR";
        if (g.Rec().Active()) {
            int s = int(g.Rec().Seconds());
            wchar_t t[64];
            swprintf(t, 64, L"Stop recording (%d:%02d)\tR", s / 60, s % 60);
            rec = t;
        }
        AppendMenuW(m, MF_STRING, kMenuRecord, pro ? rec.c_str() : L"Start recording (Pro)…");
        AppendMenuW(m, MF_STRING | (g.ObsMode() ? MF_CHECKED : 0), kMenuObs,
                    pro ? L"OBS mode\tO" : L"OBS mode (Pro)…");
        AppendMenuW(m, MF_STRING, kMenuZoom,
                    !pro ? L"Zoom into a region (Pro)…" : g.Zoomed() ? L"Zoom out\tZ" : L"Zoom into a region\tZ");
        AppendMenuW(m, MF_STRING | (g.DrawMode() ? MF_CHECKED : 0), kMenuDraw,
                    pro ? L"Draw on screen\tD" : L"Draw on screen (Pro)…");
    }
    AppendMenuW(m, MF_STRING, kMenuMediaFolder, L"Open screenshots folder");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    if (Licensed()) {
        AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"AirGlass Pro — thank you!");
        // Refund eligibility, shown honestly so buyers can quote it when asking for a refund.
        if (!cfg_.licenseKey.empty() && cfg_.proUnlocked > 0) {
            long long days = (_time64(nullptr) - cfg_.proUnlocked) / 86400;
            int sessionsLeft = license::kRefundSessions - cfg_.proSessions;
            wchar_t line[160];
            if (days < license::kRefundDays && sessionsLeft > 0)
                swprintf(line, 160, L"Refundable: %d of %d sessions used, %lld days left", cfg_.proSessions,
                         license::kRefundSessions, (long long)license::kRefundDays - days);
            else
                swprintf(line, 160, L"Pro sessions: %d (refund period over)", cfg_.proSessions);
            AppendMenuW(m, MF_STRING | MF_GRAYED, 0, line);
        }
    }
#ifndef AIRGLASS_ALWAYS_PRO
    else {
        int left = TrialDaysLeft();
        wchar_t line[96];
        if (cfg_.trialStarted == 0)
            swprintf(line, 96, L"Free %d-day Pro trial starts when you mirror", license::kTrialDays);
        else if (left > 0)
            swprintf(line, 96, L"Pro trial: %d day%s left", left, left == 1 ? L"" : L"s");
        else
            swprintf(line, 96, L"Pro trial ended");
        AppendMenuW(m, MF_STRING | MF_GRAYED, 0, line);
        AppendMenuW(m, MF_STRING, kMenuPro, (std::wstring(L"Buy AirGlass Pro (") + license::kPrice + L")…").c_str());
    }
#endif
    bool ask = askOn_;
    AppendMenuW(m, MF_STRING | (ask ? MF_CHECKED : 0) | (cfg_.askManaged ? MF_GRAYED : 0), kMenuAskDevices,
                cfg_.askManaged       ? L"Ask before new devices connect (set by your organisation)"
                : ask || glass_.Pro() ? L"&Ask before new devices connect"
                                      : L"&Ask before new devices connect (Pro)…");
    if (ask || !cfg_.devices.empty())
        AppendMenuW(m, MF_STRING | (cfg_.devices.empty() ? MF_GRAYED : 0), kMenuForgetDevices,
                    L"&Forget allowed and blocked devices");
    bool pin = server_.PinRequired();
    AppendMenuW(m, MF_STRING | (pin ? MF_CHECKED : 0) | (cfg_.pinManaged ? MF_GRAYED : 0), kMenuRequirePin,
                cfg_.pinManaged       ? L"Require a code to connect (set by your organisation)"
                : pin || glass_.Pro() ? L"Require a &code to connect"
                                      : L"Require a &code to connect (Pro)…");
    if (pin || !cfg_.paired.empty())
        AppendMenuW(m, MF_STRING | (cfg_.paired.empty() ? MF_GRAYED : 0), kMenuForgetPaired,
                    L"Forget &paired devices");
    AppendMenuW(m, MF_STRING | (AutostartEnabled() ? MF_CHECKED : 0), kMenuAutostart, L"Start with Windows");
    AppendMenuW(m, MF_STRING, kMenuLogs, L"Open log folder");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, kMenuQuit, L"Quit AirGlass");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(msgWnd_);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, msgWnd_, nullptr);
    DestroyMenu(m);
    switch (cmd) {
    case kMenuAutostart: SetAutostart(!AutostartEnabled()); break;
    case kMenuLogs: ShellExecuteW(nullptr, L"open", AppDataDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
    case kMenuShow:
        for (int i = 0; i < kWindows; ++i)  // the one used last ends up in front
            if (Win(i) && Win(i) != &g) Win(i)->BringToFront();
        g.BringToFront();
        break;
    case kMenuRename: RenameReceiver(); break;
    case kMenuPro: ShowUpgrade(L"Buy once, use it on up to 3 PCs."); break;
    case kMenuShot:
        if (glass_.Pro()) TakeScreenshot(g);
        else PostMessageW(msgWnd_, kMsgUpgrade, GlassWindow::kProScreenshot, 0);
        break;
    case kMenuRecord:
        if (glass_.Pro()) ToggleRecording(g);
        else PostMessageW(msgWnd_, kMsgUpgrade, GlassWindow::kProRecord, 0);
        break;
    case kMenuObs: g.ToggleObsMode(); break;  // these ask for Pro in the free edition
    case kMenuZoom: g.ToggleRegionZoom(); break;
    case kMenuDraw: g.ToggleDrawMode(); break;
    case kMenuAskDevices:
        if (cfg_.askManaged) break;
        // Turning it on is Pro; turning it off never is, and a list that is on stays on after the trial, so it
        // never quietly stops protecting the PC.
        if (!askOn_ && !glass_.Pro()) {
            PostMessageW(msgWnd_, kMsgUpgrade, GlassWindow::kProAllowList, 0);
            break;
        }
        cfg_.askNewDevices = !askOn_;
        askOn_ = cfg_.askNewDevices;
        cfg_.Save();
        LOGI("devices: ask before new devices connect: %s", askOn_ ? "on" : "off");
        if (askOn_)
            Balloon(L"AirGlass asks first",
                    L"When a new device starts mirroring or audio, this PC asks whether to allow it.");
        break;
    case kMenuForgetDevices: {
        {
            std::lock_guard<std::mutex> lk(devMu_);
            cfg_.devices.clear();
        }
        cfg_.Save();
        LOGI("devices: list cleared");
        Balloon(L"Device list cleared", askOn_ ? L"Every device will be asked about again."
                                               : L"Devices you allowed or blocked are forgotten.");
        break;
    }
    case kMenuRequirePin:
        if (cfg_.pinManaged) break;
        // As the allow-list: turning it on is Pro, turning it off never is, and it stays on after the trial.
        if (!server_.PinRequired() && !glass_.Pro()) {
            PostMessageW(msgWnd_, kMsgUpgrade, GlassWindow::kProPin, 0);
            break;
        }
        cfg_.requirePin = !server_.PinRequired();
        cfg_.Save();
        ApplyPinRequired(cfg_.requirePin);
        if (cfg_.requirePin)
            Balloon(L"AirGlass asks for a code",
                    L"A device connecting for the first time is asked for a code shown on this PC.");
        break;
    case kMenuForgetPaired:
        cfg_.paired.clear();
        cfg_.Save();
        server_.SetPairedKeys({});
        LOGI("pin: paired devices forgotten");
        Balloon(L"Paired devices forgotten", server_.PinRequired() ? L"Every device is asked for a code again."
                                                                   : L"Devices that paired with a code are forgotten.");
        break;
    case kMenuMediaFolder: {
        std::wstring dir = MediaFile(FOLDERID_Pictures, nullptr);
        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    }
    case kMenuQuit: PostQuitMessage(0); break;
    default: break;
    }
}

LRESULT CALLBACK App::StaticProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (g_app) return g_app->Proc(h, m, w, l);
    return DefWindowProcW(h, m, w, l);
}

LRESULT App::Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == taskbarCreated_ && taskbarCreated_) {
        AddTrayIcon();
        return 0;
    }
    switch (m) {
    case kMsgTray: {
        UINT ev = LOWORD(l);
        if (ev == WM_CONTEXTMENU || ev == WM_RBUTTONUP) ShowTrayMenu();
        else if (ev == WM_LBUTTONUP || ev == NIN_SELECT || ev == NIN_KEYSELECT) {
            if (Active().Visible()) Active().BringToFront();
            else
                Balloon(L"AirGlass is ready",
                        L"On your iPhone, iPad or Mac open Screen Mirroring and choose “" + Utf8ToWide(name_) +
                            L"”.");
        }
        return 0;
    }
    case kMsgMirrorStart: {
        std::unique_ptr<StartEvent> e(reinterpret_cast<StartEvent*>(l));
#ifndef AIRGLASS_ALWAYS_PRO
        // Free Pro trial: starts with the first real session, decided before the window opens.
        if (!loopback_ && !Licensed()) {
            bool starting = cfg_.trialStarted == 0;
            if (starting) {
                cfg_.trialStarted = _time64(nullptr);
                cfg_.Save();
                LOGI("license: %d-day Pro trial started", license::kTrialDays);
            }
            bool on = TrialDaysLeft() > 0;
            SetPro(on);
            if (starting)
                Balloon(L"Your 7-day AirGlass Pro trial has started",
                        L"Move and resize the glass window, keep it on top (T), take screenshots (S), record with sound "
                        L"(R), draw (D), zoom into a region (Z) and use OBS mode (O). Full-screen mirroring stays free "
                        L"after the trial.");
            else if (!on && !cfg_.trialEndShown) {
                cfg_.trialEndShown = true;
                cfg_.Save();
                PostMessageW(msgWnd_, kMsgTrialEnded, 0, 0);
            }
        }
#endif
        int wi = e->window > 0 && e->window < kWindows && Win(e->window) ? e->window : 0;
        GlassWindow& g = *Win(wi);
        uint64_t before = live_[wi].sid;
        // Taken over: this window's previous session (the free edition's one session, or in Pro the oldest of two
        // when a third device starts), and in the free edition any other session too.
        for (int j = 0; j < kWindows; ++j) {
            uint64_t old = live_[j].sid;
            if (!old || old == e->sid || (j != wi && glass_.Pro())) continue;
            bool otherDevice = live_[j].device != e->name;
            LOGI("sessions: \"%s\" takes over window %d from session %llu (\"%s\")", e->name.c_str(), j + 1,
                 (unsigned long long)old, live_[j].device.c_str());
            if (GlassWindow* o = Win(j)) {
                if (o->Session() == old) SaveRecordingAtEnd(*o, L"Another device took over.");
                if (j != wi) {  // the server allowed two before the trial ended: the free edition keeps one
                    server_.DisconnectMirror(old);
                    o->EndSession(old);
                }
            }
            EmitStopped(old, "takeover");
            if (!glass_.Pro() && otherDevice && !twoDevicesAsked_) {
                twoDevicesAsked_ = true;
                PostMessageW(msgWnd_, kMsgUpgrade, GlassWindow::kProTwoDevices, 0);
            }
        }
        // A second window opens beside the first, and under TV Mode the first moves to the other side.
        RECT beside{};
        bool haveBeside = false;
        for (int j = 0; j < kWindows; ++j) {
            GlassWindow* o = Win(j);
            if (j == wi || !o || !o->SessionLive() || !o->Visible()) continue;
            beside = o->TvRect();
            haveBeside = true;
            GlassWindow::TvLayout t = o->CurrentTvLayout();
            if (o->TvControlled() && strcmp(o->TvModeName(), "float") == 0 && t.corner == tvDefaults_.corner) {
                t.corner ^= 1;  // br <-> bl, tr <-> tl
                o->ApplyTvLayout(t);
            }
        }
        g.BeginSession(e->sid, Utf8ToWide(e->name), e->model, haveBeside && !g.TvControlled() ? &beside : nullptr);
        if (glass_.Pro() && !loopback_ && !cfg_.licenseKey.empty() && e->sid != before) {
            ++cfg_.proSessions;  // refund policy counter (see license::kRefundSessions)
            cfg_.Save();
            LOGI("license: Pro session %d", cfg_.proSessions);
        }
        live_[wi] = Announced{e->sid, e->name, e->model, 0, 0};
        tvWin_ = wi;  // TV Mode follows the most recent session
        // The new session has the sound (the server gave it), unless the other window has the keyboard focus.
        active_ = wi;
        for (int j = 0; j < kWindows; ++j)
            if (j != wi && Win(j) && Win(j)->SessionLive() && GetForegroundWindow() == Win(j)->Hwnd()) FocusWindow(j);
        LOGI("sessions: session %llu (\"%s\") in window %d; %d mirroring at once", (unsigned long long)e->sid,
             e->name.c_str(), wi + 1, LiveCount());
        GlassWindow::TvLayout t = g.CurrentTvLayout();
        Emit(JsonWriter()
                 .Str("event", "started")
                 .Int("session", (long long)e->sid)
                 .Str("device", e->name)
                 .Str("model", e->model)
                 .Str("mode", g.TvModeName())
                 .Str("corner", kCornerNames[std::clamp(t.corner, 0, 3)])
                 .Str("size", kSizeNames[std::clamp(t.size, 0, 2)])
                 .Done());
        return 0;
    }
    case kMsgMirrorStop: {
        uint64_t sid = uint64_t(w);
        int wi = WindowOf(sid);
        if (wi >= 0) {
            SaveRecordingAtEnd(*Win(wi), L"Mirroring ended.");
            Win(wi)->EndSession(sid);
        }
        for (const Announced& a : live_)
            if (a.sid == sid) {
                EmitStopped(sid, "sender");
                break;
            }
        if (!Win(active_) || !Win(active_)->SessionLive())
            for (int j = 0; j < kWindows; ++j)
                if (Win(j) && Win(j)->SessionLive()) active_ = j;  // the tools follow the session that is left
        return 0;
    }
    case kMsgVideoSize:
        for (int i = 0; i < kWindows; ++i)
            if (Win(i)) Win(i)->SetVideoSize(uint64_t(w), int(LOWORD(l)), int(HIWORD(l)));  // the session's own window
        OnSessionSize(uint64_t(w), int(LOWORD(l)), int(HIWORD(l)));
        return 0;
    case kMsgPipeLine: {
        std::unique_ptr<std::string> line(reinterpret_cast<std::string*>(l));
        OnPipeCommand(*line);
        return 0;
    }
    case kMsgPipeClients:
        pipeClients_ = int(w);
        if (pipeClients_ > 0) {
            KillTimer(msgWnd_, kTimerTvGrace);
            for (int i = 0; i < kWindows; ++i)
                if (Win(i)) Win(i)->SetTvControlled(true);
            EmitState();  // greets the client that just connected
        } else {
            // The TV server may just be restarting: keep the TV behaviour for a few seconds.
            SetTimer(msgWnd_, kTimerTvGrace, 5000, nullptr);
        }
        return 0;
    case kMsgNowPlaying:
        // A track change arrives as several SET_PARAMETERs within a millisecond: send one event.
        SetTimer(msgWnd_, kTimerNpDebounce, 40, nullptr);
        return 0;
    case kMsgMediaDone: {
        std::unique_ptr<MediaResult> r(reinterpret_cast<MediaResult*>(l));
        JsonWriter jw;
        jw.Str("event", "mediaResult").Str("rid", r->rid).Bool("ok", r->error.empty());
        if (!r->error.empty()) jw.Str("error", r->error);
        Emit(jw.Done());
        return 0;
    }
    case WM_TIMER:
        if (w == kTimerNowPlaying) EmitNowPlaying();  // position heartbeat
        if (w == kTimerNpDebounce) {
            KillTimer(msgWnd_, kTimerNpDebounce);
            npPending_ = false;
            EmitNowPlaying();
        }
        if (w == kTimerPromoteTray && (PromoteTrayIcon() || ++promoteTries_ >= 20)) KillTimer(msgWnd_, kTimerPromoteTray);
        if (w == kTimerTvGrace) {
            KillTimer(msgWnd_, kTimerTvGrace);
            if (pipeClients_ == 0)
                for (int i = 0; i < kWindows; ++i)
                    if (Win(i)) Win(i)->SetTvControlled(false);
        }
        return 0;
    case kMsgUpgrade: {
        const wchar_t* what = w == GlassWindow::kProScreenshot ? L"Screenshots are"
                              : w == GlassWindow::kProRecord   ? L"Recording is"
                              : w == GlassWindow::kProObs      ? L"OBS mode is"
                              : w == GlassWindow::kProZoom     ? L"Region zoom is"
                              : w == GlassWindow::kProDraw     ? L"Drawing on screen is"
                              : w == GlassWindow::kProAllowList ? L"Asking before new devices connect is"
                              : w == GlassWindow::kProPin      ? L"Requiring a code to connect is"
                              : w == GlassWindow::kProTwoDevices ? L"Mirroring two devices at once is"
                                                               : L"Windowed mode is";
        std::wstring reason = std::wstring(what) + L" part of AirGlass Pro.";
        reason += cfg_.trialStarted                    ? L" Your free trial has ended."
                  : w == GlassWindow::kProTwoDevices ? L" The free edition mirrors one at a time."
                                                       : L" The free edition mirrors full screen.";
        ShowUpgrade(reason);
        return 0;
    }
    case kMsgTrialEnded:
        ShowUpgrade(L"Your 7-day Pro trial has ended. Keep the glass window, screenshots and recording; "
                    L"full screen stays free.");
        return 0;
    case kMsgRecordStopped: {
        std::unique_ptr<std::wstring> file(reinterpret_cast<std::wstring*>(l));
        Balloon(L"Recording saved", L"The device rotated, so the recording was finished. Saved to Videos\\AirGlass as " +
                                        file->substr(file->find_last_of(L'\\') + 1) + L". Press R to record again.");
        return 0;
    }
    case kMsgAskDevice: {
        std::unique_ptr<std::shared_ptr<AdmitRequest>> r(reinterpret_cast<std::shared_ptr<AdmitRequest>*>(l));
        askQueue_.push_back(*r);
        AskNextDevice();
        return 0;
    }
    case kMsgPinCode: {
        std::unique_ptr<std::string> code(reinterpret_cast<std::string*>(l));
        if (code->empty()) {
            pinCard_.Hide();
        } else {
            std::wstring wcode = Utf8ToWide(*code);
            pinCard_.Show(wcode, Utf8ToWide(cfg_.name), Active().SessionMonitor(), AirPlayServer::kPinCodeSeconds);
            crypto::SecureWipe(wcode.data(), wcode.size() * sizeof(wchar_t));
        }
        crypto::SecureWipe(*code);
        return 0;
    }
    case kMsgPinPaired: {
        std::unique_ptr<PairedDevice> d(reinterpret_cast<PairedDevice*>(l));
        if (loopback_) return 0;  // tests leave the user's config.ini alone
        auto it = cfg_.paired.find(d->key);
        if (it == cfg_.paired.end() || it->second != d->name) {
            if (it == cfg_.paired.end()) LOGI("pin: \"%s\" paired with the code; remembered", d->name.c_str());
            cfg_.paired[d->key] = d->name;
            cfg_.Save();
        }
        return 0;
    }
    case kMsgRenamed: {
        std::unique_ptr<RenameResult> r(reinterpret_cast<RenameResult*>(l));
        ApplyName(r->wanted, r->name);
        return 0;
    }
    case kMsgLicenseRevoked:
        LOGW("license: no longer valid; back to the free edition");
        cfg_.licenseKey.clear();
        cfg_.licenseInstance.clear();
        cfg_.proUnlocked = 0;
        cfg_.proSessions = 0;
        cfg_.Save();
        SetPro(false);
        Balloon(L"AirGlass Pro is no longer active",
                L"The license key was refunded or disabled. AirGlass keeps working in full-screen mode.");
        return 0;
    case kMsgShowExisting:
        if (Active().Visible()) Active().BringToFront();
        else Balloon(L"AirGlass is already running", L"Choose “" + Utf8ToWide(name_) + L"” in Screen Mirroring.");
        return 0;
    case WM_CLOSE:  // "AirGlass.exe --quit" from another process
        PostQuitMessage(0);
        return 0;
    default: break;
    }
    return DefWindowProcW(h, m, w, l);
}

void App::EmitState() {
    GlassWindow& tg = TvGlass();
    bool live = tg.SessionLive() && TvSid();
    const Announced& a = live ? live_[tvWin_] : Announced{};
    GlassWindow::TvLayout t = live ? tg.CurrentTvLayout() : tvDefaults_;
    NowPlaying np;
    bool music = server_.GetNowPlaying(np);
    Emit(JsonWriter()
             .Str("event", "state")
             .Int("version", 1)
             .Str("receiver", name_)
             .Int("session", live ? (long long)a.sid : 0)
             .Str("device", a.device)
             .Str("model", a.model)
             .Int("width", a.w)
             .Int("height", a.h)
             .Str("mode", live ? tg.TvModeName() : LayoutModeName(t.mode))
             .Str("corner", kCornerNames[std::clamp(t.corner, 0, 3)])
             .Str("size", kSizeNames[std::clamp(t.size, 0, 2)])
             .Bool("visible", live && tg.Visible())
             .Int("sessions", LiveCount())
             .Raw("nowplaying", music ? NowPlayingJson(np, false) : "null")
             .Done());
}

void App::EmitStopped(uint64_t sid, const char* reason) {
    Emit(JsonWriter().Str("event", "stopped").Int("session", (long long)sid).Str("reason", reason).Done());
    for (Announced& a : live_)
        if (a.sid == sid) a = Announced{};
    if (tvWin_ >= 0 && !live_[tvWin_].sid) {
        // TV Mode now follows the session that is left, if any, and hears about it in a fresh state event.
        tvWin_ = -1;
        for (int i = 0; i < kWindows; ++i)
            if (live_[i].sid) tvWin_ = i;
        if (tvWin_ >= 0) EmitState();
    }
}

void App::EmitLayout(int window, bool byUser) {
    GlassWindow* g = Win(window);
    if (!g || window < 0 || window >= kWindows || !live_[window].sid) return;
    GlassWindow::TvLayout t = g->CurrentTvLayout();
    RECT r = g->TvRect();
    char rect[96];
    snprintf(rect, sizeof(rect), "[%ld,%ld,%ld,%ld]", r.left, r.top, r.right - r.left, r.bottom - r.top);
    Emit(JsonWriter()
             .Str("event", "layoutChanged")
             .Int("session", (long long)live_[window].sid)
             .Str("mode", g->TvModeName())
             .Str("corner", kCornerNames[std::clamp(t.corner, 0, 3)])
             .Str("size", kSizeNames[std::clamp(t.size, 0, 2)])
             .Raw("rect", rect)
             .Str("by", byUser ? "user" : "tv")
             .Done());
}

GlassWindow& App::TvGlass() {
    GlassWindow* g = tvWin_ >= 0 ? Win(tvWin_) : nullptr;
    return g ? *g : glass_;
}

void App::EmitNowPlaying() {
    NowPlaying np;
    bool active = server_.GetNowPlaying(np);
    if (active) {
        if (npSid_ && npSid_ != np.session)
            Emit(JsonWriter().Str("event", "nowplaying").Int("session", (long long)npSid_).Bool("active", false).Done());
        npSid_ = np.session;
        Emit(NowPlayingJson(np, true));
        SetTimer(msgWnd_, kTimerNowPlaying, 5000, nullptr);
    } else if (npSid_) {
        Emit(JsonWriter().Str("event", "nowplaying").Int("session", (long long)npSid_).Bool("active", false).Done());
        npSid_ = 0;
        KillTimer(msgWnd_, kTimerNowPlaying);
    }
}

void App::OnPipeCommand(const std::string& line) {
    JsonObject o;
    if (!ParseJsonObject(line, o)) {
        LOGW("pipe: ignoring malformed command: %.200s", line.c_str());
        return;
    }
    std::string cmd = Str(o, "cmd");
    LOGD("pipe: %s", line.c_str());
    auto button = [&]() {
        auto it = o.find("button");
        if (it == o.end() || it->second.type != JsonValue::Type::String) return -1;
        return IndexOf(kButtonNames, 4, it->second.s, -1);
    };
    if (cmd == "state") {
        EmitState();
    } else if (cmd == "defaults") {
        tvDefaults_ = ParseLayout(o, tvDefaults_);
        for (int i = 0; i < kWindows; ++i)
            if (Win(i)) Win(i)->SetTvDefaults(tvDefaults_);
    } else if (cmd == "layout") {  // these drive the most recent session's window
        GlassWindow& tg = TvGlass();
        if (tg.SessionLive()) tg.ApplyTvLayout(ParseLayout(o, tg.CurrentTvLayout()));
    } else if (cmd == "highlight") {
        TvGlass().SetTvHighlight(button());
    } else if (cmd == "press") {
        TvGlass().PressTvButton(button());
    } else if (cmd == "stop") {
        GlassWindow& tg = TvGlass();
        if (tg.SessionLive() && TvSid()) {
            uint64_t sid = tg.Session();
            LOGI("pipe: TV Mode stopped the mirroring session");
            server_.DisconnectMirror(sid);
            tg.EndSession(sid);
            EmitStopped(sid, "tv");
        }
    } else if (cmd == "media") {
        MediaJob job;
        job.action = Str(o, "action");
        job.rid = Str(o, "rid");
        auto v = o.find("value");
        if (v != o.end() && v->second.type == JsonValue::Type::Number) job.value = v->second.n;
        std::lock_guard<std::mutex> lk(mediaMu_);
        mediaQ_.push_back(job);
        mediaCv_.notify_one();
    } else {
        LOGW("pipe: unknown command \"%s\"", cmd.c_str());
    }
}

void App::ShowUpgrade(const std::wstring& reason) {
    if (Licensed() || upgradeOpen_) return;
    if (loopback_) {  // automated tests: never pop a dialog
        LOGI("license: upgrade dialog suppressed in loopback mode");
        return;
    }
    upgradeOpen_ = true;
    std::string key, instance;
    HWND owner = Active().Visible() ? Active().Hwnd() : msgWnd_;
    if (license::ShowUpgradeDialog(owner, reason, &key, &instance)) {
        cfg_.licenseKey = key;
        cfg_.licenseInstance = instance;
        cfg_.proUnlocked = _time64(nullptr);
        cfg_.proSessions = 0;
        cfg_.Save();
        SetPro(true);
        LOGI("license: AirGlass Pro unlocked");
    }
    upgradeOpen_ = false;
}

int App::TrialDaysLeft() const {
    if (cfg_.trialStarted == 0) return license::kTrialDays;
    long long left = cfg_.trialStarted + (long long)license::kTrialDays * 86400 - _time64(nullptr);
    return left <= 0 ? 0 : int((left + 86399) / 86400);
}

// "<known folder>\AirGlass\AirGlass 2026-11-15 10.42.31<tag><ext>"; with ext == nullptr, the folder itself.
std::wstring App::MediaFile(REFKNOWNFOLDERID folder, const wchar_t* ext, const wchar_t* tag) {
    std::wstring dir;
    PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(folder, KF_FLAG_CREATE, nullptr, &p))) dir = p;
    CoTaskMemFree(p);
    if (dir.empty()) dir = AppDataDir();
    dir += L"\\AirGlass";
    CreateDirectoryW(dir.c_str(), nullptr);
    if (!ext) return dir;
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[128];
    swprintf(name, 128, L"\\AirGlass %04d-%02d-%02d %02d.%02d.%02d%ls%ls", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
             t.wSecond, tag, ext);
    return dir + name;
}

// The second window's files say so, so two devices' screenshots or recordings never share a name.
void App::TakeScreenshot(GlassWindow& g) {
    if (!glass_.Pro() || !g.Visible()) return;
    g.RequestScreenshot(MediaFile(FOLDERID_Pictures, L".png", &g == &glass_ ? L"" : L" (2)"));
}

void App::ToggleRecording(GlassWindow& g) {
    if (!glass_.Pro()) return;
    if (g.Rec().Active()) {
        std::wstring file = g.Rec().Stop();
        if (!file.empty())
            Balloon(L"Recording saved", L"Saved to Videos\\AirGlass as " + file.substr(file.find_last_of(L'\\') + 1) + L".");
        return;
    }
    std::string err;
    std::wstring file = MediaFile(FOLDERID_Videos, L".mp4", &g == &glass_ ? L"" : L" (2)");
    if (g.Rec().Start(file, &err))
        Balloon(L"Recording", std::wstring(g.Rec().HasSound() ? L"Recording the mirrored screen and its sound"
                                                              : L"Recording the mirrored screen (it sends no sound)") +
                                  L" to Videos\\AirGlass. Press R or use the tray menu to stop.");
    else
        Balloon(L"Couldn't start recording", Utf8ToWide(err));
}

// A recording ends with its session (or when another device takes the window over).
void App::SaveRecordingAtEnd(GlassWindow& g, const wchar_t* why) {
    if (!g.Rec().Active()) return;
    std::wstring file = g.Rec().Stop();
    if (!file.empty())
        Balloon(L"Recording saved", std::wstring(why) + L" Saved to Videos\\AirGlass as " +
                                        file.substr(file.find_last_of(L'\\') + 1) + L".");
}

void App::OnSessionSize(uint64_t sid, int w, int h) {
    for (int i = 0; i < kWindows; ++i) {
        if (live_[i].sid != sid) continue;
        live_[i].w = w;
        live_[i].h = h;
        if (i == tvWin_)  // TV Mode hears about the session it follows
            Emit(JsonWriter().Str("event", "size").Int("session", (long long)sid).Int("width", w).Int("height", h).Done());
    }
}

// ---------------------------------------------------------------------------------------------
// Mirroring windows

bool App::CreateGlass(int i) {
    GlassWindow::Options opt;
    opt.pinned = cfg_.pinned;
    opt.longPortrait = cfg_.longPortrait;
    opt.longLandscape = cfg_.longLandscape;
    opt.obs = cfg_.obsMode;
    opt.index = i;
    if (i == 1) glass2_ = std::make_unique<GlassWindow>();
    GlassWindow* g = Win(i);
    if (!g || !g->Create(inst_, iconBig_, iconSmall_, opt)) {
        if (i == 1) {
            LOGE("ui: the second mirroring window couldn't be created; one device at a time");
            glass2_.reset();
        }
        return false;
    }
    if (i == 1) LOGI("ui: second mirroring window ready (two devices at once)");
    g->onUserClose = [this, g] {
        uint64_t sid = g->Session();
        server_.DisconnectMirror(sid);
        g->EndSession(sid);
        if (!sid) return;
        for (const Announced& a : live_)
            if (a.sid == sid) {
                Emit(JsonWriter().Str("event", "userClosed").Int("session", (long long)sid).Done());
                EmitStopped(sid, "user");
                break;
            }
    };
    g->onTvLayoutChanged = [this, i](bool byUser) { EmitLayout(i, byUser); };
    // Posted, so the dialog never opens inside the glass window's own input handling.
    g->onUpgradeRequested = [this](int feature) { PostMessageW(msgWnd_, kMsgUpgrade, WPARAM(feature), 0); };
    g->onScreenshotKey = [this, g] { TakeScreenshot(*g); };
    g->onRecordToggle = [this, g] { ToggleRecording(*g); };
    g->onScreenshot = [this](bool ok, const std::wstring& path) {
        if (ok)
            Balloon(L"Screenshot saved", L"Saved to Pictures\\AirGlass as " + path.substr(path.find_last_of(L'\\') + 1) +
                                             L", and the picture is on the clipboard.");
        else
            Balloon(L"Screenshot copied", L"The picture is on the clipboard (the PNG file couldn't be saved).");
    };
    g->Rec().onAutoStop = [this](const std::wstring& path, const std::string&) {
        PostMessageW(msgWnd_, kMsgRecordStopped, 0, reinterpret_cast<LPARAM>(new std::wstring(path)));
    };
    g->onContentSize = [this](uint64_t sid, int w, int h) { OnSessionSize(sid, w, h); };
    g->onOptionsChanged = [this, g] {
        auto o = g->CurrentOptions();
        cfg_.pinned = o.pinned;
        cfg_.longPortrait = o.longPortrait;
        cfg_.longLandscape = o.longLandscape;
        cfg_.obsMode = o.obs;
        cfg_.Save();
    };
    g->onActivated = [this, i] { FocusWindow(i); };
    g->SetTvDefaults(tvDefaults_);
    if (pipeClients_ > 0) g->SetTvControlled(true);
    return true;
}

int App::WindowOf(uint64_t sid) {
    for (int i = 0; i < kWindows; ++i)
        if (sid && Win(i) && Win(i)->SessionLive() && Win(i)->Session() == sid) return i;
    return -1;
}

int App::LiveCount() {
    int n = 0;
    for (int i = 0; i < kWindows; ++i)
        if (Win(i) && Win(i)->SessionLive()) ++n;
    return n;
}

GlassWindow& App::Active() {
    GlassWindow* g = Win(active_);
    if (g && g->SessionLive()) return *g;
    for (int i = 0; i < kWindows; ++i)
        if (Win(i) && Win(i)->SessionLive()) return *Win(i);
    return glass_;
}

void App::FocusWindow(int i) {
    GlassWindow* g = Win(i);
    if (!g || !g->SessionLive()) return;
    if (active_ != i && LiveCount() > 1) LOGI("sessions: window %d has the focus; the tools and the sound follow it", i + 1);
    active_ = i;
    server_.SetMirrorAudioFocus(g->Session());
}

// Pro mirrors two devices at once (the second window is made the first time Pro is on); the free edition one, and
// a new sender takes over.
void App::SetPro(bool on) {
    if (on && !glass2_) CreateGlass(1);
    glass_.SetPro(on);
    if (glass2_) glass2_->SetPro(on);
    UpdateMirrorSinks();
}

void App::UpdateMirrorSinks() {
    std::vector<FrameSink*> sinks{&glass_};
    if (glass_.Pro() && glass2_) sinks.push_back(glass2_.get());
    server_.SetMirrorSinks(sinks);
}

// Re-checks a stored key once per start. Only a definite "invalid" answer (refund, disabled key)
// locks Pro again; being offline keeps it.
void App::CheckLicenseInBackground() {
    if (cfg_.licenseKey.empty() || cfg_.licenseInstance.empty()) return;
    std::thread([key = cfg_.licenseKey, instance = cfg_.licenseInstance, target = msgWnd_] {
        Sleep(15000);  // let start-up (and the network) settle first
        license::Result r = license::Validate(key, instance);
        if (r.ok && !r.valid) PostMessageW(target, kMsgLicenseRevoked, 0, 0);
    }).detach();
}

void App::MediaLoop() {
    for (;;) {
        MediaJob job;
        {
            std::unique_lock<std::mutex> lk(mediaMu_);
            mediaCv_.wait(lk, [this] { return mediaStop_ || !mediaQ_.empty(); });
            if (mediaStop_) return;
            job = mediaQ_.front();
            mediaQ_.pop_front();
        }
        std::string err = server_.MediaCommand(job.action, job.value);
        LOGI("media: %s%s%s", job.action.c_str(), err.empty() ? "" : " -> ", err.c_str());
        PostMessageW(msgWnd_, kMsgMediaDone, 0, reinterpret_cast<LPARAM>(new MediaResult{job.rid, err}));
    }
}

int App::Run(HINSTANCE inst, bool background, bool loopback) {
    inst_ = inst;
    g_app = this;
    cfg_.Load();
    name_ = cfg_.name;
    if (loopback) {
        cfg_.port = 7010;
        name_ += " (loopback test)";
    }
    iconBig_ = CreateAppIcon(GetSystemMetrics(SM_CXICON) * 2);
    iconSmall_ = CreateAppIcon(GetSystemMetrics(SM_CXSMICON));

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = StaticProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"AirGlassTray";
    RegisterClassExW(&wc);
    msgWnd_ = CreateWindowExW(0, L"AirGlassTray", L"AirGlass", 0, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");

    if (!CreateGlass(0)) {
        MessageBoxW(nullptr, L"AirGlass could not initialise Direct3D 11 / DirectComposition.", L"AirGlass",
                    MB_ICONERROR);
        return 1;
    }

    loopback_ = loopback;
#ifdef AIRGLASS_ALWAYS_PRO
    bool pro = true;  // personal build (python build.py --pro)
#else
    bool pro = Licensed() || TrialDaysLeft() > 0;  // the trial starts with the first mirroring session
    // Loopback tests exercise the windowed UI unless AIRGLASS_DEBUG_FREE asks for the free edition.
    if (loopback) pro = !GetEnvironmentVariableW(L"AIRGLASS_DEBUG_FREE", nullptr, 0);
    else CheckLicenseInBackground();
#endif
    SetPro(pro);
    LOGI("edition: %s", Licensed() ? "Pro" : pro ? "Pro trial" : "free (full screen only)");

    if (!loopback) {
        std::vector<std::string> taken = MdnsResponder::BrowseReceiverNames(1.2);
        std::string candidate = UniqueReceiverName(cfg_.name, taken);
        if (candidate != cfg_.name) {
            LOGI("receiver name \"%s\" is already used by another device here; using \"%s\"", cfg_.name.c_str(),
                 candidate.c_str());
            cfg_.name = candidate;
            cfg_.Save();
        }
        name_ = cfg_.name;
        LOGI("%zu other AirPlay receivers on the network", taken.size());
    }

    ri_ = BuildIdentity();
    const ReceiverInfo& ri = ri_;
    if (cfg_.nameManaged) LOGI("receiver: name set machine-wide in %s", WideToUtf8(MachineConfigPath()).c_str());
    LOGI("receiver \"%s\" id %s, display %dx%d@%d maxFPS %d", ri.name.c_str(), ri.DeviceId().c_str(), ri.width,
         ri.height, ri.refresh, ri.maxFps);

    AirPlayServer::Events ev;
    HWND target = msgWnd_;
    ev.mirrorStarted = [target](uint64_t sid, const std::string& name, const std::string& model, int window) {
        PostMessageW(target, kMsgMirrorStart, 0, reinterpret_cast<LPARAM>(new StartEvent{sid, name, model, window}));
    };
    ev.mirrorStopped = [target](uint64_t sid) { PostMessageW(target, kMsgMirrorStop, WPARAM(sid), 0); };
    ev.videoSize = [target](uint64_t sid, int w, int h) {
        PostMessageW(target, kMsgVideoSize, WPARAM(sid), MAKELPARAM(std::min(w, 65535), std::min(h, 65535)));
    };
    ev.nowPlayingChanged = [this, target] {
        if (!npPending_.exchange(true)) PostMessageW(target, kMsgNowPlaying, 0, 0);
    };
    askOn_ = cfg_.askNewDevices;
    if (askOn_)
        LOGI("devices: asking before new devices connect%s, %zu known", cfg_.askManaged ? " (set machine-wide)" : "",
             cfg_.devices.size());
    ev.admit = [this](const std::string& id, const std::string& name, const std::function<bool()>& gone) {
        return Admit(id, name, gone);
    };
    ev.pinCode = [target](const std::string& code) {
        PostMessageW(target, kMsgPinCode, 0, reinterpret_cast<LPARAM>(new std::string(code)));
    };
    ev.pinPaired = [target](const std::string& key, const std::string& name) {
        PostMessageW(target, kMsgPinPaired, 0, reinterpret_cast<LPARAM>(new PairedDevice{key, name}));
    };
    // PIN pairing. Loopback tests never need a code unless AIRGLASS_TEST_PIN (4 digits) asks for the PIN path,
    // and they start with no paired devices.
    bool pinOn = cfg_.requirePin;
    if (loopback) {
        wchar_t tp[16] = L"";
        DWORD n = GetEnvironmentVariableW(L"AIRGLASS_TEST_PIN", tp, 16);
        std::string testPin = n == 4 ? WideToUtf8(tp) : std::string();
        pinOn = testPin.size() == 4 &&
                std::all_of(testPin.begin(), testPin.end(), [](char c) { return c >= '0' && c <= '9'; });
        if (pinOn) server_.SetTestPin(testPin);
    } else {
        std::vector<std::string> keys;
        for (const auto& kv : cfg_.paired) keys.push_back(kv.first);
        server_.SetPairedKeys(keys);
    }
    server_.SetPinRequired(pinOn);
    ri_.pin = pinOn;
    if (pinOn)
        LOGI("pin: a code is required to connect%s, %zu paired devices", cfg_.pinManaged ? " (set machine-wide)" : "",
             loopback ? size_t(0) : cfg_.paired.size());
    if (!server_.Start(ri, uint16_t(cfg_.port), &glass_, &audio_, ev)) {
        MessageBoxW(nullptr, L"AirGlass could not open its network port.", L"AirGlass", MB_ICONERROR);
        return 1;
    }
    UpdateMirrorSinks();  // Pro: two at once

    // TV Mode control channel.
    mediaThread_ = std::thread(&App::MediaLoop, this);
    pipe_.Start(loopback ? L"\\\\.\\pipe\\AirGlass.Control.Loopback" : L"\\\\.\\pipe\\AirGlass.Control",
                [target](const std::string& line) {
                    PostMessageW(target, kMsgPipeLine, 0, reinterpret_cast<LPARAM>(new std::string(line)));
                },
                [target](int clients) { PostMessageW(target, kMsgPipeClients, WPARAM(clients), 0); });

    AddTrayIcon();
    if (!loopback) {
        if (!StartMdns()) {
            LOGE("mDNS responder failed to start; the receiver will not be discoverable");
            Balloon(L"AirGlass: discovery problem",
                    L"Could not open the Bonjour/mDNS port (UDP 5353). Devices may not see this PC.");
        }
    }
    if (!loopback && !cfg_.welcomed) {
        // First run (normally launched by the installer): pin the tray icon and say hello properly.
        cfg_.welcomed = true;
        cfg_.Save();
        SetTimer(msgWnd_, kTimerPromoteTray, 1000, nullptr);
        if (background) {
            Balloon(L"AirGlass is ready",
                    L"On your iPhone, iPad or Mac open Screen Mirroring and choose “" + Utf8ToWide(name_) + L"”.");
        } else {
            bool autostart = AutostartEnabled();
            ui::ShowWelcome(nullptr, iconBig_, Utf8ToWide(name_), &autostart);
            if (autostart != AutostartEnabled()) SetAutostart(autostart);
        }
    } else if (!loopback && !background) {
        Balloon(L"AirGlass is ready",
                L"On your iPhone, iPad or Mac open Screen Mirroring and choose “" + Utf8ToWide(name_) + L"”.");
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    LOGI("shutting down");
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = msgWnd_;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    pipe_.Stop();
    {
        std::lock_guard<std::mutex> lk(mediaMu_);
        mediaStop_ = true;
        mediaCv_.notify_all();
    }
    if (mediaThread_.joinable()) mediaThread_.join();
    if (renameThread_.joinable()) renameThread_.join();
    mdns_.Stop();
    server_.Stop();
    audio_.Stop();
    if (glass2_) glass2_->Destroy();
    glass_.Destroy();
    DestroyWindow(msgWnd_);
    g_app = nullptr;
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int) {
    std::wstring cmd = cmdLine ? cmdLine : L"";
    bool background = cmd.find(L"--background") != std::wstring::npos;

    if (cmd.find(L"--write-icon") != std::wstring::npos) {
        size_t p = cmd.find(L"--write-icon") + 12;
        std::wstring path = cmd.substr(p);
        while (!path.empty() && (path.front() == L' ' || path.front() == L'"')) path.erase(path.begin());
        while (!path.empty() && (path.back() == L' ' || path.back() == L'"')) path.pop_back();
        return WriteIconFile(path) ? 0 : 1;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    if (cmd.find(L"--selftest") != std::wstring::npos) {
        LogInit(AppDataDir() + L"\\selftest.log", LogLevel::Debug);
        bool ok = crypto::SelfTest();
        LogShutdown();
        return ok ? 0 : 2;
    }

    if (cmd.find(L"--quit") != std::wstring::npos) {
        HWND other = FindWindowW(L"AirGlassTray", nullptr);
        if (!other) return 1;
        PostMessageW(other, WM_CLOSE, 0, 0);
        for (int i = 0; i < 50 && IsWindow(other); ++i) Sleep(100);
        return IsWindow(other) ? 2 : 0;
    }

    bool loopback = cmd.find(L"--loopback") != std::wstring::npos;
    HANDLE single = CreateMutexW(nullptr, TRUE, loopback ? L"Local\\AirGlass.Loopback" : L"Local\\AirGlass.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = loopback ? nullptr : FindWindowW(L"AirGlassTray", nullptr);
        if (other) PostMessageW(other, kMsgShowExisting, 0, 0);
        return 0;
    }

    Config peek;
    peek.Load();
    bool debug = peek.debugLog || cmd.find(L"--debug") != std::wstring::npos;
    LogInit(AppDataDir() + (loopback ? L"\\airglass-loopback.log" : L"\\airglass.log"),
            debug ? LogLevel::Debug : LogLevel::Info);
    LOGI("AirGlass starting (%s%s)", background ? "background" : "interactive", loopback ? ", loopback test mode" : "");
    {
        wchar_t ws[128] = L"?", dk[128] = L"?";
        DWORD need = 0;
        GetUserObjectInformationW(GetProcessWindowStation(), UOI_NAME, ws, sizeof(ws), &need);
        GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, dk, sizeof(dk), &need);
        DWORD session = 0;
        ProcessIdToSessionId(GetCurrentProcessId(), &session);
        LOGI("desktop %s\\%s, session %lu", WideToUtf8(ws).c_str(), WideToUtf8(dk).c_str(), session);
    }
    if (loopback) net::SetLoopbackOnly(true);

    if (!crypto::SelfTest()) {
        MessageBoxW(nullptr, L"AirGlass crypto self-test failed. See the log for details.", L"AirGlass", MB_ICONERROR);
        return 1;
    }
    net::Startup();
    int rc;
    {
        App app;
        rc = app.Run(inst, background, loopback);
    }
    net::Cleanup();
    LogShutdown();
    if (single) CloseHandle(single);
    return rc;
}
