#include "DesktopDuplicator.h"

#include "../util/Log.h"

namespace celmon {

Status DesktopDuplicator::Init(D3DContext& d3d) {
    Reset();
    d3d_ = &d3d;
    HRESULT hr;
    ComPtr<IDXGIOutput5> out5;
    if (SUCCEEDED(d3d.output.As(&out5))) {
        const DXGI_FORMAT formats[] = {DXGI_FORMAT_B8G8R8A8_UNORM};
        hr = out5->DuplicateOutput1(d3d.device.Get(), 0, ARRAYSIZE(formats), formats, &dup_);
    } else {
        hr = d3d.output->DuplicateOutput(d3d.device.Get(), &dup_);
    }
    lastError_ = hr;
    if (FAILED(hr)) {
        if (hr == E_ACCESSDENIED)
            return Status::Error(L"Captura negada pelo Windows (tela segura/UAC ou bloqueio ativo). Tentando novamente...");
        if (hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE)
            return Status::Error(L"Limite de capturas simultâneas do Windows atingido (outro app está duplicando a tela).");
        if (hr == DXGI_ERROR_UNSUPPORTED)
            return Status::Error(L"Desktop Duplication não suportado nesta GPU/monitor.");
        return Status::Error(L"Falha ao iniciar a captura: " + HrText(hr));
    }

    DXGI_OUTDUPL_DESC desc;
    dup_->GetDesc(&desc);
    width_ = desc.ModeDesc.Width;
    height_ = desc.ModeDesc.Height;
    if (desc.Rotation != DXGI_MODE_ROTATION_IDENTITY && desc.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED)
        Log::Warn("monitor is rotated by Windows (%d); use a native portrait mode instead", int(desc.Rotation));

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = width_;
    td.Height = height_;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    hr = d3d.device->CreateTexture2D(&td, nullptr, &frame_);
    if (FAILED(hr)) return Status::Error(L"Falha ao alocar textura de captura: " + HrText(hr));
    Log::Info("duplication started %ux%u (desktop image in system memory: %d)", width_, height_,
              int(desc.DesktopImageInSystemMemory));
    return Status::Ok();
}

void DesktopDuplicator::Reset() {
    dup_.Reset();
    frame_.Reset();
    hasFrame_ = false;
}

DesktopDuplicator::Result DesktopDuplicator::Acquire(UINT timeoutMs) {
    if (!dup_) return Result::AccessLost;
    DXGI_OUTDUPL_FRAME_INFO info;
    ComPtr<IDXGIResource> resource;
    HRESULT hr = dup_->AcquireNextFrame(timeoutMs, &info, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return Result::Timeout;
    lastError_ = hr;
    if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL) return Result::AccessLost;
    if (FAILED(hr)) return Result::Error;

    // Pointer: position/visibility and (rarely) a new shape.
    if (info.LastMouseUpdateTime.QuadPart != 0) {
        pointer_.visible = info.PointerPosition.Visible != FALSE;
        pointer_.position = info.PointerPosition.Position;
    }
    if (info.PointerShapeBufferSize > 0) {
        pointer_.shape.resize(info.PointerShapeBufferSize);
        UINT needed = 0;
        DXGI_OUTDUPL_POINTER_SHAPE_INFO si;
        if (SUCCEEDED(dup_->GetFramePointerShape(UINT(pointer_.shape.size()), pointer_.shape.data(), &needed, &si))) {
            pointer_.shapeType = si.Type;
            pointer_.width = si.Width;
            pointer_.height = si.Height;
            pointer_.pitch = si.Pitch;
            pointer_.hotspot = si.HotSpot;
            ++pointer_.shapeVersion;
        }
    }

    Result result = Result::PointerOnly;
    if (info.LastPresentTime.QuadPart != 0 || !hasFrame_) {
        ComPtr<ID3D11Texture2D> tex;
        if (SUCCEEDED(resource.As(&tex))) {
            d3d_->context->CopyResource(frame_.Get(), tex.Get());
            hasFrame_ = true;
            result = Result::NewFrame;
        }
    }
    dup_->ReleaseFrame();
    return result;
}

}  // namespace celmon
