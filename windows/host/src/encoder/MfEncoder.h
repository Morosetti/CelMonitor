// Hardware H.264/HEVC encoding through a Media Foundation asynchronous MFT (NVIDIA/AMD/Intel via the same code).
// Input is an NV12 D3D11 texture on the same device as the capture: no CPU copies.
#pragma once

#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <codecapi.h>
#include <strmif.h>

#include <atomic>
#include <map>
#include <mutex>

#include "VideoEncoder.h"

namespace celmon {

class MfEncoder : public IVideoEncoder {
public:
    MfEncoder();
    ~MfEncoder() override;

    Status Init(D3DContext& d3d, const EncoderConfig& cfg, OutputCallback onOutput) override;
    bool ReadyForInput() const override { return needInput_ > 0 && !failed_; }
    Status Encode(ID3D11Texture2D* nv12, uint64_t ptsUs, uint64_t captureTimeUs, bool forceKeyframe) override;
    Status SetBitrate(uint32_t kbps) override;
    void Shutdown() override;
    std::wstring Name() const override { return name_; }
    bool Failed() const override { return failed_; }

    // Called by the event callback (MF work-queue thread).
    void OnEvent(IMFMediaEvent* ev);

private:
    class EventCallback;
    Status ConfigureTypes();
    void DrainOutput();
    void Fail(const wchar_t* what, HRESULT hr);

    EncoderConfig cfg_;
    OutputCallback onOutput_;
    ComPtr<IMFTransform> mft_;
    ComPtr<ICodecAPI> codecApi_;
    ComPtr<IMFMediaEventGenerator> events_;
    ComPtr<IMFDXGIDeviceManager> devManager_;
    ComPtr<EventCallback> callback_;
    UINT resetToken_ = 0;
    DWORD inputId_ = 0, outputId_ = 0;
    bool providesSamples_ = true;
    DWORD outputBufferSize_ = 0;
    std::wstring name_;
    std::vector<uint8_t> lastConfig_;  // parameter sets, re-sent before IDRs that lack them

    std::mutex mftLock_;
    std::atomic<int> needInput_{0};
    std::atomic<bool> failed_{false};
    std::atomic<bool> running_{false};
    std::mutex timesLock_;
    std::map<uint64_t, std::pair<uint64_t, LARGE_INTEGER>> inFlight_;  // pts -> (capture time, submit QPC)
};

}  // namespace celmon
