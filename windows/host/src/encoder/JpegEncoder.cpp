#include "JpegEncoder.h"

#include <algorithm>
#include <cstring>

#include "../pipeline/VideoPipeline.h"

#pragma comment(lib, "windowscodecs.lib")

namespace celmon {

float JpegEncoder::QualityFor(uint32_t quality) {
    // q50 (default) -> 0.65: text stays readable; q90 -> 0.85; q30 -> 0.55. Above ~0.9 pictures grow fast for
    // little visible gain, and every byte costs time on the receiving device's CPU.
    return 0.40f + 0.50f * std::clamp<uint32_t>(quality, 1, 100) / 100.0f;
}

void JpegEncoder::SetQuality(uint32_t quality) {
    quality_ = QualityFor(quality);
    qualityChanged_ = true;  // resend everything so the whole screen has the new quality
}

Status JpegEncoder::Init(D3DContext& d3d, const EncoderConfig& cfg, OutputCallback onOutput) {
    d3d_ = &d3d;
    cfg_ = cfg;
    onOutput_ = std::move(onOutput);
    quality_ = QualityFor(cfg.quality);
    prev_.clear();

    UINT support = 0;
    if (FAILED(d3d.device->CheckFormatSupport(DXGI_FORMAT_NV12, &support)) || !(support & D3D11_FORMAT_SUPPORT_CPU_LOCKABLE))
        return Status::Error(L"A GPU não permite ler imagens NV12 pela CPU (necessário para o modo JPEG).");
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = cfg.width;
    td.Height = cfg.height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_NV12;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    HRESULT hr = d3d.device->CreateTexture2D(&td, nullptr, &staging_);
    if (FAILED(hr)) return Status::Error(L"Falha ao alocar a imagem de leitura do modo JPEG: " + HrText(hr));
    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_));
    if (FAILED(hr)) return Status::Error(L"Windows Imaging Component indisponível: " + HrText(hr));
    return Status::Ok();
}

HRESULT JpegEncoder::EncodeTile(const BYTE* y, const BYTE* uv, UINT pitch, UINT width, UINT rows, std::vector<uint8_t>& out) {
    ComPtr<IStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    ComPtr<IWICPlanarBitmapFrameEncode> planar;
    HRESULT hr = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
    if (SUCCEEDED(hr)) hr = wic_->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc);
    if (SUCCEEDED(hr)) hr = enc->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = enc->CreateNewFrame(&frame, &props);
    if (SUCCEEDED(hr)) {
        PROPBAG2 opt[2] = {};
        VARIANT val[2];
        opt[0].pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VariantInit(&val[0]);
        val[0].vt = VT_R4;
        val[0].fltVal = quality_;
        opt[1].pstrName = const_cast<LPOLESTR>(L"JpegYCrCbSubsampling");
        VariantInit(&val[1]);
        val[1].vt = VT_UI1;
        val[1].bVal = WICJpegYCrCbSubsampling420;
        hr = props->Write(2, opt, val);
    }
    if (SUCCEEDED(hr)) hr = frame->Initialize(props.Get());
    if (SUCCEEDED(hr)) hr = frame->SetSize(width, rows);
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;  // required before WritePixels, even for planar input
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
    if (SUCCEEDED(hr)) hr = frame.As(&planar);
    if (SUCCEEDED(hr)) {
        WICBitmapPlane planes[2] = {
            {GUID_WICPixelFormat8bppY, const_cast<BYTE*>(y), pitch, pitch * rows},
            {GUID_WICPixelFormat16bppCbCr, const_cast<BYTE*>(uv), pitch, pitch * (rows / 2)},
        };
        hr = planar->WritePixels(rows, planes, 2);
    }
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = enc->Commit();
    HGLOBAL mem = nullptr;
    if (SUCCEEDED(hr)) hr = GetHGlobalFromStream(stream.Get(), &mem);
    STATSTG st = {};
    if (SUCCEEDED(hr)) hr = stream->Stat(&st, STATFLAG_NONAME);
    if (FAILED(hr)) return hr;
    const auto* bytes = static_cast<const uint8_t*>(GlobalLock(mem));
    out.assign(bytes, bytes + st.cbSize.QuadPart);
    GlobalUnlock(mem);
    return S_OK;
}

Status JpegEncoder::Encode(ID3D11Texture2D* nv12, uint64_t ptsUs, uint64_t captureTimeUs, bool forceKeyframe) {
    uint64_t t0 = NowUs();
    d3d_->context->CopyResource(staging_.Get(), nv12);
    D3D11_MAPPED_SUBRESOURCE m = {};
    HRESULT hr = d3d_->context->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &m);  // waits for the GPU conversion
    if (FAILED(hr)) {
        failed_ = true;
        return Status::Error(L"Falha ao ler a imagem da GPU: " + HrText(hr));
    }
    const uint32_t w = cfg_.width, h = cfg_.height, pitch = m.RowPitch;
    const auto* y = static_cast<const BYTE*>(m.pData);
    const BYTE* uv = y + size_t(pitch) * h;  // NV12: the interleaved chroma plane follows the luma rows

    // What changed since the last picture sent, in bands of kBandRows rows: for each band the span of columns that
    // differ (prev_ holds that picture: w*h luma bytes, then w*h/2 chroma bytes). Everything when the device needs
    // a complete picture.
    const bool full = forceKeyframe || qualityChanged_.exchange(false) || prev_.size() != size_t(w) * h * 3 / 2;
    if (full) prev_.assign(size_t(w) * h * 3 / 2, 0);
    uint8_t* prevY = prev_.data();
    uint8_t* prevUV = prevY + size_t(w) * h;
    struct Region { uint32_t x0, x1, y0, y1; };
    std::vector<Region> runs;  // consecutive changed bands, with the union of their column spans
    for (uint32_t b = 0; b < h; b += kBandRows) {
        const uint32_t e = std::min(h, b + kBandRows);
        uint32_t lo = full ? 0 : w, hi = full ? w : 0;
        auto span = [&](const uint8_t* was, const uint8_t* now) {
            if (memcmp(was, now, w) == 0) return;
            uint32_t i = 0, j = w;
            while (i < lo && was[i] == now[i]) ++i;
            while (j > hi && was[j - 1] == now[j - 1]) --j;
            lo = std::min(lo, i);
            hi = std::max(hi, j);
        };
        if (!full) {
            for (uint32_t r = b; r < e; ++r) span(prevY + size_t(r) * w, y + size_t(r) * pitch);
            for (uint32_t r = b / 2; r < e / 2; ++r) span(prevUV + size_t(r) * w, uv + size_t(r) * pitch);
        }
        if (lo >= hi) continue;
        for (uint32_t r = b; r < e; ++r) memcpy(prevY + size_t(r) * w, y + size_t(r) * pitch, w);
        for (uint32_t r = b / 2; r < e / 2; ++r) memcpy(prevUV + size_t(r) * w, uv + size_t(r) * pitch, w);
        // Columns in whole 16-pixel MCUs (chroma bytes come in Cb/Cr pairs, so spans are aligned the same way).
        lo = lo / kBandRows * kBandRows;
        hi = std::min(w, (hi + kBandRows - 1) / kBandRows * kBandRows);
        if (!runs.empty() && runs.back().y1 == b) {
            runs.back().y1 = e;
            runs.back().x0 = std::min(runs.back().x0, lo);
            runs.back().x1 = std::max(runs.back().x1, hi);
        } else {
            runs.push_back({lo, hi, b, e});
        }
    }
    if (runs.size() > kMaxTiles / 2) {  // changes scattered everywhere: one region covering them all
        Region all = runs.front();
        for (auto& r : runs) { all.x0 = std::min(all.x0, r.x0); all.x1 = std::max(all.x1, r.x1); all.y1 = r.y1; }
        runs = {all};
    }

    // Tiles: each region split into near-equal parts of at most kMaxTilePixels, in whole bands. Small tiles are
    // decoded in software in parallel on every core of the device; big ones went to a slow, serial hardware decoder
    // on the tested tablet (TI OMAP4: ~65 ms per picture whatever its size).
    struct Tile { Region r; std::vector<uint8_t> jpeg; };
    std::vector<Tile> tiles;
    for (const auto& r : runs) {
        uint32_t maxRows = std::max(kBandRows, kMaxTilePixels / (r.x1 - r.x0) / kBandRows * kBandRows);
        uint32_t len = r.y1 - r.y0;
        uint32_t parts = (len + maxRows - 1) / maxRows;
        uint32_t step = ((len + parts - 1) / parts + kBandRows - 1) / kBandRows * kBandRows;
        for (uint32_t s = r.y0; s < r.y1; s += step) tiles.push_back({{r.x0, r.x1, s, std::min(r.y1, s + step)}, {}});
    }
    for (auto& t : tiles) {
        const Region& r = t.r;
        hr = EncodeTile(y + size_t(r.y0) * pitch + r.x0, uv + size_t(r.y0 / 2) * pitch + r.x0, pitch, r.x1 - r.x0,
                        r.y1 - r.y0, t.jpeg);
        if (FAILED(hr)) break;
    }
    d3d_->context->Unmap(staging_.Get(), 0);
    if (FAILED(hr)) {
        failed_ = true;
        prev_.clear();
        return Status::Error(L"Falha ao comprimir a imagem em JPEG: " + HrText(hr));
    }

    // Payload: u16 tileCount, u16 reserved, tileCount x {u16 x, u16 y, u16 w, u16 h, u32 length}, then the JPEGs.
    // No tiles (nothing visible changed): an empty update, which the pipeline does not send.
    EncodedFrame f;
    if (!tiles.empty()) {
        size_t total = 4 + 12 * tiles.size();
        for (auto& t : tiles) total += t.jpeg.size();
        f.data.reserve(total);
        auto u16 = [&](uint32_t v) { f.data.push_back(uint8_t(v)); f.data.push_back(uint8_t(v >> 8)); };
        auto u32 = [&](uint32_t v) { u16(v & 0xFFFF); u16(v >> 16); };
        u16(uint32_t(tiles.size()));
        u16(0);
        for (auto& t : tiles) {
            u16(t.r.x0); u16(t.r.y0); u16(t.r.x1 - t.r.x0); u16(t.r.y1 - t.r.y0);
            u32(uint32_t(t.jpeg.size()));
        }
        for (auto& t : tiles) f.data.insert(f.data.end(), t.jpeg.begin(), t.jpeg.end());
    }
    f.key = full;  // a complete picture: the device can start from it
    f.ptsUs = ptsUs;
    f.captureTimeUs = captureTimeUs;
    f.encodeUs = uint32_t(NowUs() - t0);
    onOutput_(std::move(f));
    return Status::Ok();
}

}  // namespace celmon
