// BGRA → NV12 (BT.709 limited range) and optional scaling on the GPU's video engine (ID3D11VideoProcessor).
#pragma once

#include <map>

#include "../capture/D3DContext.h"

namespace celmon {

class ColorConverter {
public:
    Status Init(D3DContext& d3d, uint32_t inW, uint32_t inH, uint32_t outW, uint32_t outH);
    Status Convert(ID3D11Texture2D* bgra, ID3D11Texture2D* nv12);

private:
    ComPtr<ID3D11VideoDevice> vdev_;
    ComPtr<ID3D11VideoContext> vctx_;
    ComPtr<ID3D11VideoProcessorEnumerator> enum_;
    ComPtr<ID3D11VideoProcessor> vp_;
    std::map<ID3D11Texture2D*, ComPtr<ID3D11VideoProcessorInputView>> inViews_;
    std::map<ID3D11Texture2D*, ComPtr<ID3D11VideoProcessorOutputView>> outViews_;
    uint32_t inW_ = 0, inH_ = 0, outW_ = 0, outH_ = 0;
};

}  // namespace celmon
