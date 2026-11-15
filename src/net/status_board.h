// Read-only session status for LAN clients that can't use the control pipe (e.g. a TV app showing the
// PC's HDMI picture while it mirrors). The app publishes one JSON object whenever the mirroring or music
// session changes; HTTP clients read it with GET /status, or long-poll with ?since=<seq>.
// Off unless config.ini has [status] http=1 (see docs/status-http.md).
#pragma once

#include <condition_variable>
#include <mutex>
#include <string>

class StatusBoard {
public:
    // json: one object without the "seq" key (it's added here). Any thread.
    void Publish(const std::string& json) {
        std::lock_guard<std::mutex> lk(mu_);
        if (json == json_) return;  // heartbeats that change nothing don't wake the pollers
        json_ = json;
        ++seq_;
        cv_.notify_all();
    }
    // Waits up to waitMs for a seq other than `since` (no wait when since < 0 or it already differs),
    // checking `cancel` every 250 ms. Returns the current object with its seq.
    template <typename Cancel>
    std::string Read(long long since, int waitMs, Cancel cancel) {
        std::unique_lock<std::mutex> lk(mu_);
        for (int waited = 0; since >= 0 && since == seq_ && waited < waitMs && !cancel(); waited += 250)
            cv_.wait_for(lk, std::chrono::milliseconds(250));
        if (json_.size() <= 2) return "{\"seq\":" + std::to_string(seq_) + "}";   // nothing published yet
        return "{\"seq\":" + std::to_string(seq_) + "," + json_.substr(1);
    }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::string json_ = "{}";
    long long seq_ = 0;
};
