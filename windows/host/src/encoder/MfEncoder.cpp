#include "MfEncoder.h"

#include <mferror.h>

#include "../util/Log.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

namespace celmon {

namespace {

std::string Narrow(const std::wstring& w) {
    char buf[256];
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, buf, sizeof(buf), nullptr, nullptr);
    return buf;
}

HRESULT SetCodecValue(ICodecAPI* api, const GUID& key, ULONG value) {
    VARIANT v = {};
    v.vt = VT_UI4;
    v.ulVal = value;
    return api->SetValue(&key, &v);
}

bool Trace() {
    static const bool on = GetEnvironmentVariableA("CELMON_TRACE_ENCODER", nullptr, 0) > 0;
    return on;
}

HRESULT SetCodecBool(ICodecAPI* api, const GUID& key, bool value) {
    VARIANT v = {};
    v.vt = VT_BOOL;
    v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    return api->SetValue(&key, &v);
}

}  // namespace

// Re-arms BeginGetEvent after each event. Holds only a weak link to the encoder (cleared on Shutdown), because
// an outstanding BeginGetEvent keeps this object alive after the encoder is gone.
class MfEncoder::EventCallback : public IMFAsyncCallback {
public:
    EventCallback(MfEncoder* owner, IMFMediaEventGenerator* gen) : owner_(owner), gen_(gen) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFAsyncCallback)) {
            *ppv = static_cast<IMFAsyncCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = --refs_;
        if (r == 0) delete this;
        return r;
    }
    STDMETHODIMP GetParameters(DWORD*, DWORD*) override { return E_NOTIMPL; }
    STDMETHODIMP Invoke(IMFAsyncResult* result) override {
        ComPtr<IMFMediaEvent> ev;
        HRESULT hr = gen_->EndGetEvent(result, &ev);
        std::lock_guard<std::mutex> lock(lock_);
        if (!owner_) return S_OK;
        if (SUCCEEDED(hr)) owner_->OnEvent(ev.Get());
        else { owner_->Fail(L"EndGetEvent", hr); return S_OK; }
        if (owner_) gen_->BeginGetEvent(this, nullptr);
        return S_OK;
    }
    void Detach() {
        std::lock_guard<std::mutex> lock(lock_);
        owner_ = nullptr;
    }

private:
    std::atomic<ULONG> refs_{1};
    std::mutex lock_;
    MfEncoder* owner_;
    ComPtr<IMFMediaEventGenerator> gen_;
};

MfEncoder::MfEncoder() { MFStartup(MF_VERSION, MFSTARTUP_LITE); }

MfEncoder::~MfEncoder() {
    Shutdown();
    MFShutdown();
}

void MfEncoder::Fail(const wchar_t* what, HRESULT hr) {
    if (!failed_.exchange(true)) Log::Error("encoder failure in %s: 0x%08X", Narrow(what).c_str(), unsigned(hr));
}

Status MfEncoder::Init(D3DContext& d3d, const EncoderConfig& cfg, OutputCallback onOutput) {
    cfg_ = cfg;
    onOutput_ = std::move(onOutput);
    const GUID subtype = cfg.codec == proto::Codec::H265 ? MFVideoFormat_HEVC : MFVideoFormat_H264;
    const wchar_t* codecName = cfg.codec == proto::Codec::H265 ? L"H.265" : L"H.264";
    if (cfg.codec == proto::Codec::AV1) return Status::Error(L"AV1 ainda não é suportado pelo host.");

    // ---- find a hardware encoder on the same GPU as the captured texture
    MFT_REGISTER_TYPE_INFO inInfo = {MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO outInfo = {MFMediaType_Video, subtype};
    ComPtr<IMFAttributes> enumAttrs;
    MFCreateAttributes(&enumAttrs, 1);
    enumAttrs->SetBlob(MFT_ENUM_ADAPTER_LUID, reinterpret_cast<const UINT8*>(&d3d.adapterLuid), sizeof(LUID));
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFTEnum2(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, &inInfo,
                          &outInfo, enumAttrs.Get(), &activates, &count);
    if (FAILED(hr) || count == 0) {
        if (activates) CoTaskMemFree(activates);
        return Status::Error(std::wstring(L"Nenhum encoder de hardware ") + codecName + L" disponível na GPU " +
                             d3d.adapterName + L". Atualize o driver de vídeo ou escolha outro codec.");
    }
    for (UINT32 i = 0; i < count && !mft_; ++i) {
        WCHAR* friendly = nullptr;
        UINT32 len = 0;
        activates[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &friendly, &len);
        if (SUCCEEDED(activates[i]->ActivateObject(IID_PPV_ARGS(&mft_)))) name_ = friendly ? friendly : L"MFT";
        if (friendly) CoTaskMemFree(friendly);
    }
    for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
    CoTaskMemFree(activates);
    if (!mft_) return Status::Error(L"Falha ao ativar o encoder de hardware.");
    Log::Info("encoder: %s (%s)", Narrow(name_).c_str(), cfg.codec == proto::Codec::H265 ? "HEVC" : "H.264");

    // ---- async unlock + low latency
    ComPtr<IMFAttributes> attrs;
    if (SUCCEEDED(mft_->GetAttributes(&attrs))) {
        UINT32 isAsync = 0;
        attrs->GetUINT32(MF_TRANSFORM_ASYNC, &isAsync);
        if (!isAsync) return Status::Error(L"O encoder encontrado não é assíncrono (não suportado).");
        attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
        attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
    }
    hr = mft_->GetStreamIDs(1, &inputId_, 1, &outputId_);
    if (hr == E_NOTIMPL) { inputId_ = 0; outputId_ = 0; }

    // ---- D3D11 input without copies
    hr = MFCreateDXGIDeviceManager(&resetToken_, &devManager_);
    if (SUCCEEDED(hr)) hr = devManager_->ResetDevice(d3d.device.Get(), resetToken_);
    if (SUCCEEDED(hr)) hr = mft_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(devManager_.Get()));
    if (FAILED(hr)) return Status::Error(L"O encoder não aceitou o dispositivo Direct3D: " + HrText(hr));

    // ---- rate control and latency (unsupported knobs are logged, not fatal)
    if (SUCCEEDED(mft_.As(&codecApi_))) {
        struct { const GUID* key; ULONG value; const char* name; } knobs[] = {
            {&CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR, "CBR"},
            {&CODECAPI_AVEncCommonMeanBitRate, cfg.bitrateKbps * 1000, "bitrate"},
            {&CODECAPI_AVEncMPVDefaultBPictureCount, 0, "no B-frames"},
            {&CODECAPI_AVEncMPVGOPSize, cfg.fps * 10, "GOP"},
        };
        for (auto& k : knobs)
            if (FAILED(SetCodecValue(codecApi_.Get(), *k.key, k.value))) Log::Warn("encoder ignored setting: %s", k.name);
        if (FAILED(SetCodecBool(codecApi_.Get(), CODECAPI_AVLowLatencyMode, true))) Log::Warn("encoder has no low-latency mode");
    }

    Status s = ConfigureTypes();
    if (!s.ok) return s;

    MFT_OUTPUT_STREAM_INFO osi = {};
    mft_->GetOutputStreamInfo(outputId_, &osi);
    providesSamples_ = (osi.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    outputBufferSize_ = osi.cbSize ? osi.cbSize : cfg.width * cfg.height;

    hr = mft_.As(&events_);
    if (FAILED(hr)) return Status::Error(L"Encoder sem gerador de eventos: " + HrText(hr));
    mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    running_ = true;
    callback_.Attach(new EventCallback(this, events_.Get()));
    hr = events_->BeginGetEvent(callback_.Get(), nullptr);
    if (FAILED(hr)) return Status::Error(L"Falha ao iniciar os eventos do encoder: " + HrText(hr));
    return Status::Ok();
}

Status MfEncoder::ConfigureTypes() {
    const bool hevc = cfg_.codec == proto::Codec::H265;
    ComPtr<IMFMediaType> out;
    MFCreateMediaType(&out);
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264);
    out->SetUINT32(MF_MT_AVG_BITRATE, cfg_.bitrateKbps * 1000);
    MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, cfg_.width, cfg_.height);
    MFSetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, cfg_.fps, 1);
    MFSetAttributeRatio(out.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    out->SetUINT32(MF_MT_MPEG2_PROFILE, hevc ? eAVEncH265VProfile_Main_420_8 : eAVEncH264VProfile_High);
    // Signalled in the VUI so the phone decodes colors exactly as the converter produced them.
    out->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    out->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    out->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    out->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    HRESULT hr = mft_->SetOutputType(outputId_, out.Get(), 0);
    if (FAILED(hr))
        return Status::Error(L"O encoder recusou " + std::to_wstring(cfg_.width) + L"×" + std::to_wstring(cfg_.height) +
                             L" @ " + std::to_wstring(cfg_.fps) + L" fps: " + HrText(hr));

    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> in;
        hr = mft_->GetInputAvailableType(inputId_, i, &in);
        if (FAILED(hr)) return Status::Error(L"O encoder não aceita NV12: " + HrText(hr));
        GUID sub;
        if (FAILED(in->GetGUID(MF_MT_SUBTYPE, &sub)) || sub != MFVideoFormat_NV12) continue;
        MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, cfg_.width, cfg_.height);
        MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, cfg_.fps, 1);
        hr = mft_->SetInputType(inputId_, in.Get(), 0);
        if (FAILED(hr)) return Status::Error(L"O encoder recusou o formato de entrada NV12: " + HrText(hr));
        return Status::Ok();
    }
}

void MfEncoder::OnEvent(IMFMediaEvent* ev) {
    MediaEventType type = MEUnknown;
    ev->GetType(&type);
    HRESULT status = S_OK;
    ev->GetStatus(&status);
    if (FAILED(status)) { Fail(L"event status", status); return; }
    static std::atomic<int> traced{0};
    if (Trace() && traced++ < 60)
        Log::Info("enc event %s needInput=%d", type == METransformNeedInput ? "NeedInput" : type == METransformHaveOutput ? "HaveOutput" : "other",
                  needInput_.load());
    switch (type) {
    case METransformNeedInput: ++needInput_; break;
    case METransformHaveOutput: DrainOutput(); break;
    case MEError: Fail(L"MEError", status); break;
    default: break;
    }
}

void MfEncoder::DrainOutput() {
    EncodedFrame frame;
    {
        std::lock_guard<std::mutex> lock(mftLock_);
        if (!mft_) return;
        MFT_OUTPUT_DATA_BUFFER ob = {};
        ob.dwStreamID = outputId_;
        ComPtr<IMFSample> own;
        if (!providesSamples_) {
            ComPtr<IMFMediaBuffer> buf;
            MFCreateMemoryBuffer(outputBufferSize_, &buf);
            MFCreateSample(&own);
            own->AddBuffer(buf.Get());
            ob.pSample = own.Get();
        }
        DWORD st = 0;
        HRESULT hr = mft_->ProcessOutput(0, 1, &ob, &st);
        if (ob.pEvents) ob.pEvents->Release();
        ComPtr<IMFSample> sample;
        if (providesSamples_) sample.Attach(ob.pSample); else sample = own;
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            ComPtr<IMFMediaType> t;
            if (SUCCEEDED(mft_->GetOutputAvailableType(outputId_, 0, &t))) mft_->SetOutputType(outputId_, t.Get(), 0);
            return;
        }
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return;
        if (FAILED(hr) || !sample) { Fail(L"ProcessOutput", hr); return; }

        LONGLONG time = 0;
        sample->GetSampleTime(&time);
        frame.ptsUs = uint64_t(time / 10);
        UINT32 clean = 0;
        if (SUCCEEDED(sample->GetUINT32(MFSampleExtension_CleanPoint, &clean)) && clean) frame.key = true;

        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(sample->ConvertToContiguousBuffer(&buf))) { Fail(L"ConvertToContiguousBuffer", E_FAIL); return; }
        BYTE* p = nullptr;
        DWORD len = 0;
        if (SUCCEEDED(buf->Lock(&p, nullptr, &len))) {
            SplitAccessUnit(cfg_.codec, p, len, frame);
            buf->Unlock();
        }
    }
    if (!frame.config.empty()) lastConfig_ = frame.config;
    if (frame.key && frame.config.empty()) frame.config = lastConfig_;
    if (frame.data.empty()) return;  // parameter sets only (some encoders emit them separately)
    {
        std::lock_guard<std::mutex> lock(timesLock_);
        auto it = inFlight_.find(frame.ptsUs);
        if (it != inFlight_.end()) {
            frame.captureTimeUs = it->second.first;
            LARGE_INTEGER now, freq;
            QueryPerformanceCounter(&now);
            QueryPerformanceFrequency(&freq);
            frame.encodeUs = uint32_t((now.QuadPart - it->second.second.QuadPart) * 1000000 / freq.QuadPart);
            inFlight_.erase(inFlight_.begin(), std::next(it));
        }
    }
    static std::atomic<int> traced{0};
    if (Trace() && traced++ < 30)
        Log::Info("enc output pts=%llu key=%d bytes=%zu", frame.ptsUs, int(frame.key), frame.data.size());
    if (onOutput_) onOutput_(std::move(frame));
}

Status MfEncoder::Encode(ID3D11Texture2D* nv12, uint64_t ptsUs, uint64_t captureTimeUs, bool forceKeyframe) {
    if (failed_) return Status::Error(L"O encoder falhou e precisa ser reiniciado.");
    if (needInput_ <= 0) return Status::Error(L"Encoder ocupado.");

    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), nv12, 0, FALSE, &buffer);
    if (FAILED(hr)) return Status::Error(L"MFCreateDXGISurfaceBuffer: " + HrText(hr));
    ComPtr<IMF2DBuffer> buf2d;
    DWORD len = 0;
    if (SUCCEEDED(buffer.As(&buf2d)) && SUCCEEDED(buf2d->GetContiguousLength(&len))) buffer->SetCurrentLength(len);
    ComPtr<IMFSample> sample;
    MFCreateSample(&sample);
    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(LONGLONG(ptsUs) * 10);
    sample->SetSampleDuration(10000000LL / cfg_.fps);

    {
        std::lock_guard<std::mutex> lock(timesLock_);
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        inFlight_[ptsUs] = {captureTimeUs, now};
    }
    std::lock_guard<std::mutex> lock(mftLock_);
    if (forceKeyframe && codecApi_) SetCodecValue(codecApi_.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1);
    hr = mft_->ProcessInput(inputId_, sample.Get(), 0);
    if (FAILED(hr)) {
        Fail(L"ProcessInput", hr);
        return Status::Error(L"O encoder rejeitou o frame: " + HrText(hr));
    }
    --needInput_;
    static std::atomic<int> traced{0};
    if (Trace() && traced++ < 30) Log::Info("enc submit pts=%llu key=%d", ptsUs, int(forceKeyframe));
    return Status::Ok();
}

Status MfEncoder::SetBitrate(uint32_t kbps) {
    cfg_.bitrateKbps = kbps;
    if (!codecApi_) return Status::Error(L"Encoder sem ICodecAPI.");
    HRESULT hr = SetCodecValue(codecApi_.Get(), CODECAPI_AVEncCommonMeanBitRate, kbps * 1000);
    if (FAILED(hr)) return Status::Error(L"O encoder não permite mudar a taxa em tempo real: " + HrText(hr));
    return Status::Ok();
}

void MfEncoder::Shutdown() {
    if (!running_.exchange(false)) return;
    if (callback_) callback_->Detach();
    std::lock_guard<std::mutex> lock(mftLock_);
    if (mft_) {
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        MFShutdownObject(mft_.Get());  // releases a pending BeginGetEvent
    }
    callback_.Reset();
    events_.Reset();
    codecApi_.Reset();
    mft_.Reset();
    devManager_.Reset();
}

}  // namespace celmon
