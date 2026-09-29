// Desktop Duplication of one monitor. Each new desktop image is copied (GPU→GPU) into our own BGRA texture so the
// OS frame can be released immediately; the pipeline converts/encodes from that copy.
#pragma once

#include <cstdint>
#include <vector>

#include "D3DContext.h"

namespace celmon {

struct PointerState {
    bool visible = false;
    POINT position{};                 // relative to the monitor
    uint32_t shapeType = 0;           // DXGI_OUTDUPL_POINTER_SHAPE_TYPE_*
    uint32_t width = 0, height = 0, pitch = 0;
    POINT hotspot{};
    std::vector<uint8_t> shape;
    uint64_t shapeVersion = 0;        // increments when the shape changes
};

class DesktopDuplicator {
public:
    enum class Result { NewFrame, PointerOnly, Timeout, AccessLost, Error };

    Status Init(D3DContext& d3d);
    void Reset();

    // Waits up to timeoutMs for a desktop update. On NewFrame, Frame() holds the new image.
    Result Acquire(UINT timeoutMs);

    ID3D11Texture2D* Frame() const { return frame_.Get(); }
    uint32_t Width() const { return width_; }
    uint32_t Height() const { return height_; }
    const PointerState& Pointer() const { return pointer_; }
    bool HasFrame() const { return hasFrame_; }
    HRESULT LastError() const { return lastError_; }

private:
    D3DContext* d3d_ = nullptr;
    ComPtr<IDXGIOutputDuplication> dup_;
    ComPtr<ID3D11Texture2D> frame_;
    uint32_t width_ = 0, height_ = 0;
    bool hasFrame_ = false;
    PointerState pointer_;
    HRESULT lastError_ = S_OK;
};

}  // namespace celmon
