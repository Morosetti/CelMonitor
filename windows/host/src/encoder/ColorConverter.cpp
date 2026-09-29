#include "ColorConverter.h"

namespace celmon {

Status ColorConverter::Init(D3DContext& d3d, uint32_t inW, uint32_t inH, uint32_t outW, uint32_t outH) {
    inViews_.clear();
    outViews_.clear();
    inW_ = inW; inH_ = inH; outW_ = outW; outH_ = outH;
    HRESULT hr = d3d.device.As(&vdev_);
    if (SUCCEEDED(hr)) hr = d3d.context.As(&vctx_);
    if (FAILED(hr)) return Status::Error(L"A GPU não expõe o processador de vídeo D3D11: " + HrText(hr));

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = {};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputWidth = inW; cd.InputHeight = inH;
    cd.OutputWidth = outW; cd.OutputHeight = outH;
    cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
    hr = vdev_->CreateVideoProcessorEnumerator(&cd, &enum_);
    if (SUCCEEDED(hr)) hr = vdev_->CreateVideoProcessor(enum_.Get(), 0, &vp_);
    if (FAILED(hr)) return Status::Error(L"Falha ao criar o conversor de cor na GPU: " + HrText(hr));

    // Full-range RGB in, BT.709 studio-range YUV out (what H.264 VUI will declare).
    ComPtr<ID3D11VideoContext1> vctx1;
    if (SUCCEEDED(vctx_.As(&vctx1))) {
        vctx1->VideoProcessorSetStreamColorSpace1(vp_.Get(), 0, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        vctx1->VideoProcessorSetOutputColorSpace1(vp_.Get(), DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
    } else {
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE in = {};
        in.RGB_Range = 0;  // full
        vctx_->VideoProcessorSetStreamColorSpace(vp_.Get(), 0, &in);
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE out = {};
        out.YCbCr_Matrix = 1;  // BT.709
        out.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        vctx_->VideoProcessorSetOutputColorSpace(vp_.Get(), &out);
    }
    vctx_->VideoProcessorSetStreamAutoProcessingMode(vp_.Get(), 0, FALSE);
    vctx_->VideoProcessorSetStreamFrameFormat(vp_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);

    // Letterbox if aspect ratios differ, so the image is never stretched.
    RECT src = {0, 0, LONG(inW), LONG(inH)};
    RECT dst = {0, 0, LONG(outW), LONG(outH)};
    if (uint64_t(inW) * outH != uint64_t(inH) * outW) {
        if (uint64_t(inW) * outH > uint64_t(inH) * outW) {  // wider source
            LONG h = LONG(uint64_t(outW) * inH / inW) & ~1;
            dst.top = (LONG(outH) - h) / 2 & ~1;
            dst.bottom = dst.top + h;
        } else {
            LONG w = LONG(uint64_t(outH) * inW / inH) & ~1;
            dst.left = (LONG(outW) - w) / 2 & ~1;
            dst.right = dst.left + w;
        }
    }
    vctx_->VideoProcessorSetStreamSourceRect(vp_.Get(), 0, TRUE, &src);
    vctx_->VideoProcessorSetStreamDestRect(vp_.Get(), 0, TRUE, &dst);
    vctx_->VideoProcessorSetOutputTargetRect(vp_.Get(), TRUE, &dst);
    D3D11_VIDEO_COLOR black = {};
    black.YCbCr = {0.0625f, 0.5f, 0.5f, 1.0f};
    vctx_->VideoProcessorSetOutputBackgroundColor(vp_.Get(), TRUE, &black);
    return Status::Ok();
}

Status ColorConverter::Convert(ID3D11Texture2D* bgra, ID3D11Texture2D* nv12) {
    HRESULT hr = S_OK;
    auto& in = inViews_[bgra];
    if (!in) {
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC d = {};
        d.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        hr = vdev_->CreateVideoProcessorInputView(bgra, enum_.Get(), &d, &in);
        if (FAILED(hr)) { inViews_.erase(bgra); return Status::Error(L"Falha na view de entrada do conversor: " + HrText(hr)); }
    }
    auto& out = outViews_[nv12];
    if (!out) {
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC d = {};
        d.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        hr = vdev_->CreateVideoProcessorOutputView(nv12, enum_.Get(), &d, &out);
        if (FAILED(hr)) { outViews_.erase(nv12); return Status::Error(L"Falha na view de saída do conversor: " + HrText(hr)); }
    }
    D3D11_VIDEO_PROCESSOR_STREAM stream = {};
    stream.Enable = TRUE;
    stream.pInputSurface = in.Get();
    hr = vctx_->VideoProcessorBlt(vp_.Get(), out.Get(), 0, 1, &stream);
    if (FAILED(hr)) return Status::Error(L"Falha na conversão de cor: " + HrText(hr));
    return Status::Ok();
}

}  // namespace celmon
