#include "control_pipe.h"

#include <sddl.h>

#include <cstdio>

// ---------------------------------------------------------------------------------------------
// JSON

namespace {

struct JsonParser {
    const std::string& t;
    size_t i = 0;
    explicit JsonParser(const std::string& text) : t(text) {}

    void Ws() {
        while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\r' || t[i] == '\n')) ++i;
    }
    bool Lit(const char* s) {
        size_t n = strlen(s);
        if (t.compare(i, n, s) != 0) return false;
        i += n;
        return true;
    }
    static void PutUtf8(std::string& o, uint32_t cp) {
        if (cp < 0x80) {
            o += char(cp);
        } else if (cp < 0x800) {
            o += char(0xC0 | (cp >> 6));
            o += char(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            o += char(0xE0 | (cp >> 12));
            o += char(0x80 | ((cp >> 6) & 0x3F));
            o += char(0x80 | (cp & 0x3F));
        } else {
            o += char(0xF0 | (cp >> 18));
            o += char(0x80 | ((cp >> 12) & 0x3F));
            o += char(0x80 | ((cp >> 6) & 0x3F));
            o += char(0x80 | (cp & 0x3F));
        }
    }
    bool Hex4(uint32_t& v) {
        if (i + 4 > t.size()) return false;
        v = 0;
        for (int k = 0; k < 4; ++k) {
            char c = t[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f') v |= uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= uint32_t(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool String(std::string& out) {
        if (i >= t.size() || t[i] != '"') return false;
        ++i;
        while (i < t.size()) {
            char c = t[i++];
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i >= t.size()) return false;
            switch (t[i++]) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                uint32_t cp;
                if (!Hex4(cp)) return false;
                if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < t.size() && t[i] == '\\' && t[i + 1] == 'u') {
                    size_t save = i;
                    i += 2;
                    uint32_t lo;
                    if (Hex4(lo) && lo >= 0xDC00 && lo < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    else i = save;
                }
                PutUtf8(out, cp);
                break;
            }
            default: return false;
            }
        }
        return false;
    }
    bool Value(JsonValue& v, int depth) {
        Ws();
        if (i >= t.size()) return false;
        char c = t[i];
        if (c == '"') {
            v.type = JsonValue::Type::String;
            return String(v.s);
        }
        if (c == '{' || c == '[') {
            v.type = JsonValue::Type::Other;
            return Skip(depth);
        }
        if (Lit("true")) {
            v.type = JsonValue::Type::Bool;
            v.b = true;
            return true;
        }
        if (Lit("false")) {
            v.type = JsonValue::Type::Bool;
            v.b = false;
            return true;
        }
        if (Lit("null")) {
            v.type = JsonValue::Type::Null;
            return true;
        }
        size_t start = i;
        while (i < t.size() && (isdigit(static_cast<unsigned char>(t[i])) || t[i] == '-' || t[i] == '+' || t[i] == '.' ||
                                t[i] == 'e' || t[i] == 'E'))
            ++i;
        if (i == start) return false;
        v.type = JsonValue::Type::Number;
        v.n = strtod(t.substr(start, i - start).c_str(), nullptr);
        return true;
    }
    bool Skip(int depth) {
        if (depth > 32) return false;
        char open = t[i++];
        char close = open == '{' ? '}' : ']';
        Ws();
        if (i < t.size() && t[i] == close) {
            ++i;
            return true;
        }
        for (;;) {
            Ws();
            if (open == '{') {
                std::string k;
                if (!String(k)) return false;
                Ws();
                if (i >= t.size() || t[i] != ':') return false;
                ++i;
            }
            JsonValue v;
            if (!Value(v, depth + 1)) return false;
            Ws();
            if (i >= t.size()) return false;
            if (t[i] == ',') {
                ++i;
                continue;
            }
            if (t[i] == close) {
                ++i;
                return true;
            }
            return false;
        }
    }
};

}  // namespace

bool ParseJsonObject(const std::string& text, JsonObject& out) {
    JsonParser p(text);
    p.Ws();
    if (p.i >= text.size() || text[p.i] != '{') return false;
    ++p.i;
    p.Ws();
    if (p.i < text.size() && text[p.i] == '}') return true;
    for (;;) {
        p.Ws();
        std::string k;
        if (!p.String(k)) return false;
        p.Ws();
        if (p.i >= text.size() || text[p.i] != ':') return false;
        ++p.i;
        JsonValue v;
        if (!p.Value(v, 1)) return false;
        out[k] = std::move(v);
        p.Ws();
        if (p.i >= text.size()) return false;
        if (text[p.i] == ',') {
            ++p.i;
            continue;
        }
        return text[p.i] == '}';
    }
}

std::string JsonQuote(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) {
                char b[8];
                snprintf(b, sizeof(b), "\\u%04x", c);
                o += b;
            } else {
                o += char(c);
            }
        }
    }
    return o + "\"";
}

JsonWriter& JsonWriter::Num(const char* k, double v) {
    if (!std::isfinite(v)) return Null(k);
    char b[32];
    snprintf(b, sizeof(b), "%.6g", v);
    return Key(k).Append(b);
}

// ---------------------------------------------------------------------------------------------
// Named pipe server

bool ControlPipe::Start(const std::wstring& name, std::function<void(const std::string&)> onLine,
                        std::function<void(int)> onClients) {
    Stop();
    name_ = name;
    onLine_ = std::move(onLine);
    onClients_ = std::move(onClients);

    // Only this user (and SYSTEM) may connect; the default pipe DACL would let everyone read.
    HANDLE tok = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        DWORD len = 0;
        GetTokenInformation(tok, TokenUser, nullptr, 0, &len);
        std::vector<uint8_t> buf(len);
        LPWSTR sid = nullptr;
        if (len && GetTokenInformation(tok, TokenUser, buf.data(), len, &len) &&
            ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &sid)) {
            std::wstring sddl = L"D:P(A;;GA;;;" + std::wstring(sid) + L")(A;;GA;;;SY)";
            PSECURITY_DESCRIPTOR sd = nullptr;
            if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr))
                sd_ = sd;
            LocalFree(sid);
        }
        CloseHandle(tok);
    }
    if (!sd_) LOGW("pipe: could not build a security descriptor; using the default");

    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    running_ = true;
    acceptThread_ = std::thread(&ControlPipe::AcceptLoop, this);
    LOGI("pipe: listening on %s", WideToUtf8(name_).c_str());
    return true;
}

void ControlPipe::Stop() {
    if (!running_.exchange(false)) return;
    SetEvent(stopEvent_);
    if (acceptThread_.joinable()) acceptThread_.join();
    Reap(true);
    CloseHandle(stopEvent_);
    stopEvent_ = nullptr;
    if (sd_) {
        LocalFree(sd_);
        sd_ = nullptr;
    }
}

HANDLE ControlPipe::CreateInstance(bool first) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd_, FALSE};
    DWORD open = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0);
    return CreateNamedPipeW(name_.c_str(), open, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                            PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, sd_ ? &sa : nullptr);
}

void ControlPipe::AcceptLoop() {
    bool first = true;
    DWORD lastError = 0;
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    while (running_) {
        HANDLE h = CreateInstance(first);
        if (h == INVALID_HANDLE_VALUE) {
            DWORD e = GetLastError();
            if (e != lastError) LOGE("pipe: CreateNamedPipe failed %lu (is another AirGlass running?)", e);
            lastError = e;
            if (WaitForSingleObject(stopEvent_, 3000) == WAIT_OBJECT_0) break;
            continue;
        }
        first = false;
        lastError = 0;
        ResetEvent(ov.hEvent);
        BOOL ok = ConnectNamedPipe(h, &ov);
        DWORD err = ok ? ERROR_PIPE_CONNECTED : GetLastError();
        if (err == ERROR_IO_PENDING) {
            HANDLE waits[2] = {ov.hEvent, stopEvent_};
            DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            DWORD n = 0;
            if (w != WAIT_OBJECT_0) {
                CancelIoEx(h, &ov);
                GetOverlappedResult(h, &ov, &n, TRUE);
                CloseHandle(h);
                break;
            }
            if (!GetOverlappedResult(h, &ov, &n, FALSE)) {
                CloseHandle(h);
                continue;
            }
        } else if (err != ERROR_PIPE_CONNECTED) {
            CloseHandle(h);
            continue;
        }
        Reap(false);
        auto c = std::make_shared<Client>();
        c->pipe = h;
        int count;
        {
            std::lock_guard<std::mutex> lk(mu_);
            clients_.push_back(c);
            count = 0;
            for (auto& x : clients_) count += x->dead ? 0 : 1;
        }
        c->thread = std::thread(&ControlPipe::ClientLoop, this, c);
        ULONG pid = 0;
        GetNamedPipeClientProcessId(h, &pid);
        LOGI("pipe: client connected (pid %lu, %d connected)", pid, count);
        if (onClients_) onClients_(count);
    }
    CloseHandle(ov.hEvent);
}

void ControlPipe::ClientLoop(std::shared_ptr<Client> c) {
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::string acc;
    char buf[4096];
    while (running_ && !c->dead) {
        ResetEvent(ov.hEvent);
        DWORD n = 0;
        if (!ReadFile(c->pipe, buf, sizeof(buf), &n, &ov)) {
            if (GetLastError() != ERROR_IO_PENDING) break;
            HANDLE waits[2] = {ov.hEvent, stopEvent_};
            DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (w != WAIT_OBJECT_0) {
                CancelIoEx(c->pipe, &ov);
                GetOverlappedResult(c->pipe, &ov, &n, TRUE);
                break;
            }
            if (!GetOverlappedResult(c->pipe, &ov, &n, FALSE)) break;
        }
        acc.append(buf, n);
        size_t nl;
        while ((nl = acc.find('\n')) != std::string::npos) {
            std::string line = acc.substr(0, nl);
            acc.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty() && onLine_) onLine_(line);
        }
        if (acc.size() > (1u << 20)) {
            LOGW("pipe: client sent an over-long line; disconnecting it");
            break;
        }
    }
    CloseHandle(ov.hEvent);
    {
        std::lock_guard<std::mutex> lk(c->writeMu);
        c->dead = true;
        CloseHandle(c->pipe);
        c->pipe = INVALID_HANDLE_VALUE;
    }
    if (!running_) return;
    int count = Clients();
    LOGI("pipe: client disconnected (%d connected)", count);
    if (onClients_) onClients_(count);
}

bool ControlPipe::Write(Client& c, const std::string& line) {
    std::lock_guard<std::mutex> lk(c.writeMu);
    if (c.dead || c.pipe == INVALID_HANDLE_VALUE) return false;
    std::string data = line + "\n";
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD n = 0;
    BOOL ok = WriteFile(c.pipe, data.data(), DWORD(data.size()), &n, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ov.hEvent, 500) == WAIT_OBJECT_0) {
            ok = GetOverlappedResult(c.pipe, &ov, &n, FALSE);
        } else {
            LOGW("pipe: client is not reading its events; disconnecting it");
            CancelIoEx(c.pipe, &ov);
            GetOverlappedResult(c.pipe, &ov, &n, TRUE);
            ok = FALSE;
        }
    }
    CloseHandle(ov.hEvent);
    if (!ok || n != data.size()) {
        c.dead = true;
        CancelIoEx(c.pipe, nullptr);  // wakes the reader so the client thread exits
        return false;
    }
    return true;
}

void ControlPipe::Broadcast(const std::string& jsonLine) {
    std::vector<std::shared_ptr<Client>> cs;
    {
        std::lock_guard<std::mutex> lk(mu_);
        cs = clients_;
    }
    for (auto& c : cs)
        if (!c->dead) Write(*c, jsonLine);
}

int ControlPipe::Clients() {
    std::lock_guard<std::mutex> lk(mu_);
    int n = 0;
    for (auto& c : clients_) n += c->dead ? 0 : 1;
    return n;
}

void ControlPipe::Reap(bool all) {
    std::vector<std::shared_ptr<Client>> done;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto it = clients_.begin(); it != clients_.end();) {
            if (all || (*it)->dead) {
                done.push_back(*it);
                it = clients_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& c : done)
        if (c->thread.joinable()) c->thread.join();
}
