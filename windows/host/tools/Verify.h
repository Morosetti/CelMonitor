// Independent check of an encoded stream: decodes it with Microsoft's software H.264 decoder (not the encoder's
// vendor) and saves the last picture as PNG.
#pragma once

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wincodec.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace verify {

using Microsoft::WRL::ComPtr;

struct Result {
    int decodedFrames = 0;
    uint32_t width = 0, height = 0;
    std::string error;
};

inline bool SavePng(const wchar_t* path, const std::vector<uint8_t>& bgra, uint32_t w, uint32_t h) {
    ComPtr<IWICImagingFactory> f;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(f->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path, GENERIC_WRITE))) return false;
    if (FAILED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) || FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)))
        return false;
    if (FAILED(enc->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr))) return false;
    frame->SetSize(w, h);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    frame->SetPixelFormat(&fmt);
    if (FAILED(frame->WritePixels(h, w * 4, UINT(bgra.size()), const_cast<BYTE*>(bgra.data())))) return false;
    return SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
}

// BT.709 limited range NV12 -> BGRA.
inline void Nv12ToBgra(const uint8_t* y, const uint8_t* uv, LONG pitch, uint32_t w, uint32_t h, std::vector<uint8_t>& out) {
    out.resize(size_t(w) * h * 4);
    for (uint32_t r = 0; r < h; ++r) {
        for (uint32_t c = 0; c < w; ++c) {
            double Y = (y[r * pitch + c] - 16) * 1.164;
            double U = uv[(r / 2) * pitch + (c & ~1u)] - 128.0;
            double V = uv[(r / 2) * pitch + (c & ~1u) + 1] - 128.0;
            auto clip = [](double v) { return uint8_t(std::clamp(v, 0.0, 255.0)); };
            uint8_t* px = &out[(size_t(r) * w + c) * 4];
            px[0] = clip(Y + 2.112 * U);
            px[1] = clip(Y - 0.213 * U - 0.533 * V);
            px[2] = clip(Y + 1.793 * V);
            px[3] = 255;
        }
    }
}

inline Result DecodeFile(const wchar_t* path, const wchar_t* pngOut) {
    Result res;
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (data.empty()) { res.error = "arquivo vazio ou inexistente"; return res; }

    // Split into access units: a new unit starts at SPS or at a slice when the current unit already has a slice.
    std::vector<size_t> starts;
    for (size_t i = 0; i + 3 < data.size(); ++i)
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) { starts.push_back(i > 0 && data[i - 1] == 0 ? i - 1 : i); i += 2; }
    std::vector<std::pair<size_t, size_t>> units;
    size_t unitStart = 0;
    bool hasSlice = false;
    for (size_t k = 0; k < starts.size(); ++k) {
        size_t head = starts[k] + (data[starts[k] + 2] == 1 ? 3 : 4);
        uint8_t type = data[head] & 0x1F;
        bool slice = type == 1 || type == 5;
        if ((type == 7 || slice) && hasSlice) { units.push_back({unitStart, starts[k]}); unitStart = starts[k]; hasSlice = false; }
        if (slice) hasSlice = true;
    }
    units.push_back({unitStart, data.size()});

    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    ComPtr<IMFTransform> dec;
    if (FAILED(CoCreateInstance(CLSID_MSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dec)))) {
        res.error = "decoder H.264 da Microsoft indisponivel";
        return res;
    }
    ComPtr<IMFMediaType> in;
    MFCreateMediaType(&in);
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    dec->SetInputType(0, in.Get(), 0);
    auto setOutput = [&]() {
        for (DWORD i = 0;; ++i) {
            ComPtr<IMFMediaType> t;
            if (FAILED(dec->GetOutputAvailableType(0, i, &t))) return false;
            GUID sub;
            t->GetGUID(MF_MT_SUBTYPE, &sub);
            if (sub == MFVideoFormat_NV12) {
                dec->SetOutputType(0, t.Get(), 0);
                MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &res.width, &res.height);
                return true;
            }
        }
    };
    setOutput();

    std::vector<uint8_t> lastBgra;
    uint32_t lastW = 0, lastH = 0;
    auto pull = [&]() {
        for (;;) {
            MFT_OUTPUT_STREAM_INFO si = {};
            dec->GetOutputStreamInfo(0, &si);
            ComPtr<IMFSample> s;
            ComPtr<IMFMediaBuffer> b;
            MFCreateSample(&s);
            MFCreateMemoryBuffer(std::max<DWORD>(si.cbSize, res.width * res.height * 3 / 2), &b);
            s->AddBuffer(b.Get());
            MFT_OUTPUT_DATA_BUFFER ob = {0, s.Get(), 0, nullptr};
            DWORD st = 0;
            HRESULT hr = dec->ProcessOutput(0, 1, &ob, &st);
            if (ob.pEvents) ob.pEvents->Release();
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return;
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) { setOutput(); continue; }
            if (FAILED(hr)) { res.error = "ProcessOutput falhou"; return; }
            ++res.decodedFrames;
            ComPtr<IMFMediaBuffer> cb;
            s->ConvertToContiguousBuffer(&cb);
            ComPtr<IMF2DBuffer> b2;
            BYTE* p = nullptr;
            LONG pitch = 0;
            if (SUCCEEDED(cb.As(&b2)) && SUCCEEDED(b2->Lock2D(&p, &pitch))) {
                Nv12ToBgra(p, p + size_t(pitch) * res.height, pitch, res.width, res.height, lastBgra);
                b2->Unlock2D();
            } else if (SUCCEEDED(cb->Lock(&p, nullptr, nullptr))) {
                pitch = LONG(res.width);
                Nv12ToBgra(p, p + size_t(pitch) * res.height, pitch, res.width, res.height, lastBgra);
                cb->Unlock();
            }
            lastW = res.width;
            lastH = res.height;
        }
    };

    LONGLONG t = 0;
    for (auto& u : units) {
        ComPtr<IMFMediaBuffer> b;
        MFCreateMemoryBuffer(DWORD(u.second - u.first), &b);
        BYTE* p = nullptr;
        b->Lock(&p, nullptr, nullptr);
        memcpy(p, data.data() + u.first, u.second - u.first);
        b->Unlock();
        b->SetCurrentLength(DWORD(u.second - u.first));
        ComPtr<IMFSample> s;
        MFCreateSample(&s);
        s->AddBuffer(b.Get());
        s->SetSampleTime(t);
        t += 166666;
        HRESULT hr = dec->ProcessInput(0, s.Get(), 0);
        if (hr == MF_E_NOTACCEPTING) { pull(); hr = dec->ProcessInput(0, s.Get(), 0); }
        pull();
    }
    dec->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    pull();
    if (!lastBgra.empty() && pngOut && !SavePng(pngOut, lastBgra, lastW, lastH)) res.error = "falha ao salvar PNG";
    return res;
}

}  // namespace verify
