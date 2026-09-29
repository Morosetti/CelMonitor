// One phone connection: handshake → virtual monitor → capture/encode → stream, plus heartbeats, flow control,
// settings and (phase 2) input. Owns the monitor and the pipeline for the lifetime of the connection.
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

#include "../display/VirtualDisplay.h"
#include "../pipeline/VideoPipeline.h"
#include "../transport/Transport.h"

namespace celmon {

struct SessionSettings {
    proto::Codec codec = proto::Codec::H264;
    uint32_t fps = 60;
    uint32_t quality = 50;          // 1..100, scales the bitrate
    bool portrait = false;          // orientation of the virtual monitor
    std::optional<Mode> mode;       // explicit resolution (must be one the phone supports)
};

enum class SessionState { Handshaking, CreatingMonitor, Streaming, Ended };

struct SessionInfo {
    SessionState state = SessionState::Handshaking;
    std::wstring transport, manufacturer, model, androidVersion, displayName, encoder;
    uint32_t phoneWidth = 0, phoneHeight = 0;
    Mode mode;                      // current stream mode
    std::vector<Mode> supportedModes;
    float fps = 0;                  // frames sent per second
    uint32_t kbps = 0;              // measured throughput
    uint32_t latencyUs = 0;         // capture -> shown on the phone (approx.)
    uint32_t rttUs = 0;
    uint32_t encodeUs = 0;
};

class HostSession {
public:
    struct Events {
        std::function<void()> onChanged;                    // info changed (state/stats); any thread
        std::function<void(const Status&)> onWarning;        // non-fatal problem to show
        std::function<void(const std::wstring& why)> onEnded;
    };

    HostSession(std::unique_ptr<IConnection> conn, SessionSettings settings, Events events);
    ~HostSession();

    void Start();
    void Stop(const std::wstring& reason = L"");  // graceful: sends DISCONNECT
    bool Ended() const { return ended_; }
    SessionInfo Info() const;

    // Live changes from the PC UI.
    void SetFps(uint32_t fps);
    void SetQuality(uint32_t quality);
    void SetOrientation(bool portrait);
    Status SetResolution(const Mode& m);

private:
    void ReaderLoop();
    void WriterLoop();
    void Handle(uint16_t type, const std::vector<uint8_t>& p);
    void OnHello(const proto::Hello& h);
    void OnStreamStart(uint32_t w, uint32_t h, const std::wstring& encoder);
    void OnFrame(EncodedFrame&& f);
    void ApplyOrientation(bool portrait);
    void Send(proto::MsgType type, const std::vector<uint8_t>& payload, uint16_t flags = 0);
    void SendRaw(std::vector<uint8_t>&& message);
    void End(proto::DisconnectReason reason, const std::wstring& why, bool notifyPeer);
    uint32_t BitrateFor(uint32_t w, uint32_t h) const;
    void Changed() { if (events_.onChanged) events_.onChanged(); }

    std::unique_ptr<IConnection> conn_;
    SessionSettings settings_;
    Events events_;

    std::thread reader_, writer_;
    std::mutex queueLock_;
    std::condition_variable queueCv_;
    std::deque<std::vector<uint8_t>> queue_;
    size_t queuedBytes_ = 0;
    uint32_t sendSeq_ = 0;
    std::atomic<bool> ended_{false};
    std::atomic<bool> stopping_{false};

    VirtualDisplay display_;
    VideoPipeline pipeline_;
    proto::Hello hello_;
    uint64_t deviceKey_ = 0;

    mutable std::mutex lock_;       // guards info_ and the stream state below
    SessionInfo info_;
    std::wstring endReason_;
    uint32_t streamId_ = 0;
    bool streamReady_ = false;
    uint32_t lastFrameSent_ = 0, lastFrameAcked_ = 0;
    std::atomic<uint64_t> lastReceivedUs_{0};
    // Stats window
    uint64_t statStartUs_ = 0, statFrames_ = 0, statBytes_ = 0;
};

const wchar_t* DisconnectText(proto::DisconnectReason r);

}  // namespace celmon
