// JPEG pictures for devices without a usable hardware video decoder (Android 4.0 has no MediaCodec).
// The GPU converts to NV12 (BT.601 full range); the planes are read back and compressed by WIC's JPEG encoder
// straight from YCbCr (no RGB round trip). Synchronous: Encode() outputs the update before returning.
//
// Only what changed is sent: the picture is compared with the previous one in bands of 16 rows (one JPEG MCU row);
// each run of changed bands, cropped to the columns that changed, becomes JPEG tiles small enough for the device to
// decode them in parallel on all its cores. Payload format: docs/PROTOCOLO.md ("Codec JPEG").
#pragma once

#include <wincodec.h>

#include <atomic>
#include <vector>

#include "VideoEncoder.h"

namespace celmon {

class JpegEncoder : public IVideoEncoder {
public:
    Status Init(D3DContext& d3d, const EncoderConfig& cfg, OutputCallback onOutput) override;
    bool ReadyForInput() const override { return !failed_; }
    // forceKeyframe: send the whole picture (the device has nothing to apply updates to).
    Status Encode(ID3D11Texture2D* nv12, uint64_t ptsUs, uint64_t captureTimeUs, bool forceKeyframe) override;
    Status SetBitrate(uint32_t) override { return Status::Ok(); }  // JPEG has a quality, not a bitrate
    void SetQuality(uint32_t quality) override;
    void Shutdown() override {}
    std::wstring Name() const override { return L"JPEG (CPU)"; }
    bool Failed() const override { return failed_; }

    // WIC ImageQuality for the session quality setting (1..100).
    static float QualityFor(uint32_t quality);

    static constexpr uint32_t kBandRows = 16;     // JPEG 4:2:0 MCU height
    static constexpr uint32_t kMaxTilePixels = 1280 * 128;  // larger regions are split for parallel decoding
    static constexpr uint32_t kMaxTiles = 64;

private:
    HRESULT EncodeTile(const BYTE* y, const BYTE* uv, UINT pitch, UINT width, UINT rows, std::vector<uint8_t>& out);

    D3DContext* d3d_ = nullptr;
    EncoderConfig cfg_;
    OutputCallback onOutput_;
    ComPtr<ID3D11Texture2D> staging_;
    ComPtr<IWICImagingFactory> wic_;
    std::vector<uint8_t> prev_;   // last picture sent (NV12, tightly packed); empty = none
    std::atomic<float> quality_{0.65f};
    std::atomic<bool> qualityChanged_{false};
    std::atomic<bool> failed_{false};
};

}  // namespace celmon
