#include "VideoPipeline.h"

#include <avrt.h>
#include <timeapi.h>

#include <algorithm>
#include <chrono>

#include "../encoder/MfEncoder.h"
#include "../util/Log.h"

#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "winmm.lib")

namespace celmon {

namespace {
constexpr size_t kSurfaceCount = 4;
}  // namespace

uint64_t NowUs() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return uint64_t(now.QuadPart / freq.QuadPart * 1000000 + (now.QuadPart % freq.QuadPart) * 1000000 / freq.QuadPart);
}

uint32_t VideoPipeline::DefaultBitrateKbps(uint32_t w, uint32_t h, uint32_t fps) {
    // ~0.1 bit per pixel per frame for desktop content, clamped to what USB 2.0 and phone decoders handle well.
    double kbps = double(w) * h * fps * 0.1 / 1000.0;
    return uint32_t(std::clamp(kbps, 4000.0, 40000.0));
}

VideoPipeline::VideoPipeline() = default;
VideoPipeline::~VideoPipeline() { Stop(); }

void VideoPipeline::Start(const PipelineConfig& cfg, PipelineEvents events) {
    Stop();
    cfg_ = cfg;
    events_ = std::move(events);
    fps_ = cfg.fps;
    keyframeRequested_ = true;
    running_ = true;
    thread_ = std::thread(&VideoPipeline::Run, this);
}

void VideoPipeline::Stop() {
    if (!running_.exchange(false)) {
        if (thread_.joinable()) thread_.join();
        return;
    }
    if (thread_.joinable()) thread_.join();
}

void VideoPipeline::SetBitrate(uint32_t kbps) { pendingBitrate_ = kbps; }

PipelineStats VideoPipeline::Stats() const {
    std::lock_guard<std::mutex> lock(statsLock_);
    return stats_;
}

std::wstring VideoPipeline::EncoderName() const {
    std::lock_guard<std::mutex> lock(statsLock_);
    return encoderName_;
}

void VideoPipeline::ReportError(const Status& s) {
    if (s.message == lastError_) return;  // don't flood the UI while retrying
    lastError_ = s.message;
    char buf[512];
    WideCharToMultiByte(CP_UTF8, 0, s.message.c_str(), -1, buf, sizeof(buf), nullptr, nullptr);
    Log::Error("pipeline: %s", buf);
    if (events_.onError) events_.onError(s);
}

bool VideoPipeline::Setup() {
    Teardown();
    d3d_ = D3DContext{};
    Status s = d3d_.Create(cfg_.gdiName);
    if (!s.ok) { ReportError(s); return false; }
    s = dup_.Init(d3d_);
    if (!s.ok) { ReportError(s); return false; }
    return SetupEncoder(dup_.Width(), dup_.Height());
}

bool VideoPipeline::SetupEncoder(uint32_t w, uint32_t h) {
    if (encoder_) encoder_->Shutdown();
    encoder_.reset();
    w &= ~1u;  // NV12 needs even dimensions
    h &= ~1u;
    Status s = converter_.Init(d3d_, dup_.Width(), dup_.Height(), w, h);
    if (!s.ok) { ReportError(s); return false; }

    {
        std::lock_guard<std::mutex> lock(surfaceLock_);
        surfaces_.assign(kSurfaceCount, {});
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_NV12;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        for (auto& surf : surfaces_) {
            HRESULT hr = d3d_.device->CreateTexture2D(&td, nullptr, &surf.tex);
            if (FAILED(hr)) { ReportError(Status::Error(L"Falha ao alocar superfícies NV12: " + HrText(hr))); return false; }
        }
    }

    EncoderConfig ec;
    ec.codec = cfg_.codec;
    ec.width = w;
    ec.height = h;
    ec.fps = fps_;
    ec.bitrateKbps = cfg_.bitrateKbps ? cfg_.bitrateKbps : DefaultBitrateKbps(w, h, fps_);
    auto enc = std::make_unique<MfEncoder>();
    s = enc->Init(d3d_, ec, [this](EncodedFrame&& f) { OnEncoded(std::move(f)); });
    if (!s.ok) { ReportError(s); return false; }
    encoder_ = std::move(enc);
    streamW_ = w;
    streamH_ = h;
    {
        std::lock_guard<std::mutex> lock(statsLock_);
        encoderName_ = encoder_->Name();
        stats_.width = w;
        stats_.height = h;
    }
    lastError_.clear();
    keyframeRequested_ = true;
    Log::Info("stream %ux%u @%u fps, %u kbps", w, h, ec.fps, ec.bitrateKbps);
    if (events_.onStreamStart) events_.onStreamStart(w, h, encoder_->Name());
    return true;
}

void VideoPipeline::Teardown() {
    if (encoder_) encoder_->Shutdown();
    encoder_.reset();
    {
        std::lock_guard<std::mutex> lock(surfaceLock_);
        surfaces_.clear();
    }
    dup_.Reset();
}

VideoPipeline::Surface* VideoPipeline::FreeSurface() {
    std::lock_guard<std::mutex> lock(surfaceLock_);
    for (auto& s : surfaces_)
        if (s.busyPts == 0) return &s;
    return nullptr;
}

bool VideoPipeline::EncodeCurrent(bool forceKey) {
    Surface* surf = FreeSurface();
    if (!surf || !dup_.HasFrame()) return false;
    Status s = converter_.Convert(dup_.Frame(), surf->tex.Get());
    if (!s.ok) { ReportError(s); return false; }
    static const bool trace = GetEnvironmentVariableA("CELMON_TRACE_GPU", nullptr, 0) > 0;
    if (trace) {  // diagnostics: how long the GPU takes to execute the conversion we just queued
        D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
        ComPtr<ID3D11Query> q;
        d3d_.device->CreateQuery(&qd, &q);
        d3d_.context->End(q.Get());
        uint64_t t0 = NowUs();
        BOOL done = FALSE;
        while (d3d_.context->GetData(q.Get(), &done, sizeof(done), 0) != S_OK) Sleep(0);
        Log::Info("gpu convert completed after %.2f ms", (NowUs() - t0) / 1000.0);
    }
    // Regular synthetic timestamps (n / fps). Wall-clock timestamps with gaps (static desktop, GPU contention) make
    // the NVIDIA MFT throttle its input requests to the observed gap, which then feeds on itself (measured: 1 fps).
    // The real capture time travels separately for latency measurement.
    uint64_t captureUs = NowUs();
    uint64_t pts = ++frameIndex_ * 1000000ull / std::max<uint32_t>(1, fps_);
    {
        std::lock_guard<std::mutex> lock(surfaceLock_);
        surf->busyPts = pts;
    }
    s = encoder_->Encode(surf->tex.Get(), pts, captureUs, forceKey);
    d3d_.context->Flush();  // start the copy/convert on the GPU now instead of whenever the context next flushes
    if (!s.ok) {
        std::lock_guard<std::mutex> lock(surfaceLock_);
        surf->busyPts = 0;
        ReportError(s);
        return false;
    }
    return true;
}

void VideoPipeline::OnEncoded(EncodedFrame&& f) {
    {
        // The encoder works in order: every input up to this pts has been consumed.
        std::lock_guard<std::mutex> lock(surfaceLock_);
        for (auto& s : surfaces_)
            if (s.busyPts != 0 && s.busyPts <= f.ptsUs) s.busyPts = 0;
    }
    {
        std::lock_guard<std::mutex> lock(statsLock_);
        ++stats_.framesEncoded;
        stats_.bytesEncoded += f.data.size() + f.config.size();
        stats_.lastEncodeUs = f.encodeUs;
    }
    if (events_.onFrame) events_.onFrame(std::move(f));
}

void VideoPipeline::Run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    timeBeginPeriod(1);
    DWORD taskIndex = 0;
    HANDLE task = AvSetMmThreadCharacteristicsW(L"Capture", &taskIndex);

    bool ready = false;
    bool pendingFrame = false;  // newest desktop image not yet encoded
    uint64_t lastEncodeUs = 0;
    while (running_) {
        if (!ready) {
            ready = Setup();
            if (!ready) { Sleep(500); continue; }
        }
        if (encoder_->Failed()) {
            ReportError(Status::Error(L"O encoder de vídeo falhou; reiniciando..."));
            if (!SetupEncoder(dup_.Width(), dup_.Height())) { Sleep(1000); continue; }
        }
        if (uint32_t kbps = pendingBitrate_.exchange(0)) encoder_->SetBitrate(kbps);

        // Capture and encode overlap: the desktop is drained continuously (keeping only the newest image) while the
        // encoder works; as soon as the encoder, the receiver and the fps cap allow, the newest image is encoded, so
        // the phone always gets the most recent picture (lowest latency) instead of a queued one.
        // FPS cap with jitter tolerance: a frame may come a bit early (e.g. 14 ms apart at 60 Hz) without being held
        // back a whole vsync; the average still cannot exceed the cap by more than 25%.
        uint64_t minSpacing = 800000 / std::max<uint32_t>(1, fps_);
        auto canEncode = [&] {
            if (events_.canSend && !events_.canSend()) return false;
            if (!encoder_->ReadyForInput() || !FreeSurface()) return false;
            return !lastEncodeUs || NowUs() - lastEncodeUs >= minSpacing;
        };
        // Never block inside AcquireNextFrame: while it waits it holds the D3D device lock that the encoder MFT also
        // needs (measured: encode time 3 ms -> 20 ms). Poll without waiting and sleep outside instead.
        switch (dup_.Acquire(0)) {
        case DesktopDuplicator::Result::NewFrame:
            { std::lock_guard<std::mutex> lock(statsLock_); ++stats_.desktopFrames; }
            pendingFrame = true;
            break;
        case DesktopDuplicator::Result::PointerOnly:
        case DesktopDuplicator::Result::Timeout:
            if (!pendingFrame && !keyframeRequested_) { Sleep(1); continue; }
            break;
        case DesktopDuplicator::Result::AccessLost: {
            // Mode change, secure desktop (UAC/lock screen), fullscreen app switching...
            uint32_t oldW = dup_.Width(), oldH = dup_.Height();
            Status s = dup_.Init(d3d_);
            if (!s.ok) {
                ReportError(s);
                if (dup_.LastError() != E_ACCESSDENIED) ready = false;  // output gone: redo everything
                Sleep(200);
                break;
            }
            if (dup_.Width() != oldW || dup_.Height() != oldH) {
                Log::Info("resolution changed %ux%u -> %ux%u", oldW, oldH, dup_.Width(), dup_.Height());
                if (!SetupEncoder(dup_.Width(), dup_.Height())) ready = false;
            }
            keyframeRequested_ = true;
            continue;
        }
        case DesktopDuplicator::Result::Error:
            ReportError(Status::Error(L"Erro na captura: " + HrText(dup_.LastError())));
            ready = false;
            Sleep(200);
            continue;
        }

        // The phone needs a keyframe but the screen is static: re-encode the last image.
        if (keyframeRequested_ && dup_.HasFrame()) pendingFrame = true;
        if (!pendingFrame) continue;
        if (canEncode()) {
            if (EncodeCurrent(keyframeRequested_.exchange(false))) {
                pendingFrame = false;
                lastEncodeUs = NowUs();
            }
        } else {
            {
                std::lock_guard<std::mutex> lock(statsLock_);
                bool flowBlocked = events_.canSend && !events_.canSend();
                ++(flowBlocked ? stats_.waitsFlow : stats_.waitsEncoder);
            }
            Sleep(1);
        }
    }
    Teardown();
    if (task) AvRevertMmThreadCharacteristics(task);
    timeEndPeriod(1);
    CoUninitialize();
}

}  // namespace celmon
