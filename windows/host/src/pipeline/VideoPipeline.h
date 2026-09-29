// Capture → GPU color conversion → hardware encode, on its own thread.
// Recovers by itself from mode changes, secure-desktop interruptions and encoder failures, reporting each problem.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "../capture/DesktopDuplicator.h"
#include "../encoder/ColorConverter.h"
#include "../encoder/VideoEncoder.h"

namespace celmon {

struct PipelineConfig {
    std::wstring gdiName;          // monitor to capture
    proto::Codec codec = proto::Codec::H264;
    uint32_t fps = 60;
    uint32_t bitrateKbps = 12000;  // 0 = derive from resolution/fps
};

struct PipelineEvents {
    // New stream geometry (first start and after resolution changes). Pipeline thread.
    std::function<void(uint32_t width, uint32_t height, const std::wstring& encoderName)> onStreamStart;
    // Encoded frame. Encoder thread.
    std::function<void(EncodedFrame&&)> onFrame;
    // Problem worth showing to the user; the pipeline keeps retrying. Pipeline thread.
    std::function<void(const Status&)> onError;
    // Flow control: return false to pause capture (receiver has too many frames in flight).
    std::function<bool()> canSend;
};

struct PipelineStats {
    uint64_t framesEncoded = 0;
    uint64_t bytesEncoded = 0;
    uint32_t lastEncodeUs = 0;
    uint32_t width = 0, height = 0;
    // Diagnostics: why the loop did not encode.
    uint64_t desktopFrames = 0;   // new desktop images from Desktop Duplication
    uint64_t waitsFlow = 0;       // receiver window full
    uint64_t waitsEncoder = 0;    // encoder or NV12 surfaces busy
    uint64_t waitsPacing = 0;     // fps cap
};

class VideoPipeline {
public:
    VideoPipeline();
    ~VideoPipeline();

    void Start(const PipelineConfig& cfg, PipelineEvents events);
    void Stop();
    bool Running() const { return running_; }

    void RequestKeyframe() { keyframeRequested_ = true; }
    void SetFps(uint32_t fps) { fps_ = fps; }
    void SetBitrate(uint32_t kbps);
    PipelineStats Stats() const;
    std::wstring EncoderName() const;

    static uint32_t DefaultBitrateKbps(uint32_t w, uint32_t h, uint32_t fps);

private:
    struct Surface {
        ComPtr<ID3D11Texture2D> tex;
        uint64_t busyPts = 0;  // 0 = free
    };

    void Run();
    bool Setup();              // D3D + duplication + converter + encoder; false = retry later
    bool SetupEncoder(uint32_t w, uint32_t h);
    void Teardown();
    bool EncodeCurrent(bool forceKey);
    Surface* FreeSurface();
    void OnEncoded(EncodedFrame&& f);
    void ReportError(const Status& s);

    PipelineConfig cfg_;
    PipelineEvents events_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::atomic<bool> keyframeRequested_{true};
    std::atomic<uint32_t> fps_{60};
    std::atomic<uint32_t> pendingBitrate_{0};

    D3DContext d3d_;
    DesktopDuplicator dup_;
    ColorConverter converter_;
    std::unique_ptr<IVideoEncoder> encoder_;
    std::mutex surfaceLock_;
    std::vector<Surface> surfaces_;
    uint32_t streamW_ = 0, streamH_ = 0;
    uint64_t frameIndex_ = 0;

    mutable std::mutex statsLock_;
    PipelineStats stats_;
    std::wstring encoderName_;
    std::wstring lastError_;
};

uint64_t NowUs();

}  // namespace celmon
