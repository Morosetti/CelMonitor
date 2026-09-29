// D3D11 device created on the GPU that owns a given monitor (found by its GDI name, e.g. \\.\DISPLAY3).
// Capture, color conversion and hardware encoding all share this device, so frames never leave the GPU.
#pragma once

#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <string>

#include "../display/VirtualDisplay.h"

namespace celmon {

using Microsoft::WRL::ComPtr;

struct D3DContext {
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput1> output;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    LUID adapterLuid{};
    std::wstring adapterName;

    // Finds the output named gdiName on any adapter and creates a multithread-protected, video-capable device on it.
    Status Create(const std::wstring& gdiName);
};

std::wstring HrText(HRESULT hr);

}  // namespace celmon
