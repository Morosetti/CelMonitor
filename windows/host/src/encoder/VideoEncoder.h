// Encoder interface. New backends (NVENC/AMF/QSV native, AV1...) implement this; see docs/DESENVOLVIMENTO.md.
#pragma once

#include <functional>
#include <string>

#include "../capture/D3DContext.h"
#include "EncodedFrame.h"

namespace celmon {

struct EncoderConfig {
    proto::Codec codec = proto::Codec::H264;
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t fps = 60;
    uint32_t bitrateKbps = 12000;
};

class IVideoEncoder {
public:
    using OutputCallback = std::function<void(EncodedFrame&&)>;
    virtual ~IVideoEncoder() = default;

    // onOutput may run on an encoder-owned thread.
    virtual Status Init(D3DContext& d3d, const EncoderConfig& cfg, OutputCallback onOutput) = 0;
    // True when the encoder can take another frame right now.
    virtual bool ReadyForInput() const = 0;
    // nv12 must stay untouched until a frame with ptsUs >= this one is output.
    virtual Status Encode(ID3D11Texture2D* nv12, uint64_t ptsUs, uint64_t captureTimeUs, bool forceKeyframe) = 0;
    virtual Status SetBitrate(uint32_t kbps) = 0;
    virtual void Shutdown() = 0;
    virtual std::wstring Name() const = 0;
    // Set when the encoder hit an unrecoverable error (reported to the UI, pipeline restarts it).
    virtual bool Failed() const = 0;
};

}  // namespace celmon
