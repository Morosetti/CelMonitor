#include "D3DContext.h"

#include <cstdio>

#include "../util/Log.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace celmon {

// A game saturating the GPU starves our copy/convert/encode submissions (measured: seconds-long stalls at 95% GPU).
// Raise our GPU scheduling priority like other streaming hosts do. HIGH/REALTIME classes need admin rights;
// without them we fall back to the best class Windows allows and report it in the log.
void RaiseGpuPriority(ID3D11Device* device) {
    ComPtr<IDXGIDevice> dxgi;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi)))) {
        HRESULT hr = dxgi->SetGPUThreadPriority(7);
        if (FAILED(hr)) hr = dxgi->SetGPUThreadPriority(1);
        INT prio = 0;
        dxgi->GetGPUThreadPriority(&prio);
        Log::Info("GPU thread priority %d", prio);
    }
    using SetClassFn = LONG(APIENTRY*)(HANDLE, INT);
    auto fn = reinterpret_cast<SetClassFn>(GetProcAddress(GetModuleHandleW(L"gdi32.dll"), "D3DKMTSetProcessSchedulingPriorityClass"));
    if (!fn) return;
    // D3DKMT_SCHEDULINGPRIORITYCLASS: 3 = ABOVE_NORMAL, 4 = HIGH (REALTIME=5 is avoided: it can starve DWM itself).
    for (INT cls : {4, 3}) {
        if (fn(GetCurrentProcess(), cls) >= 0) {
            Log::Info("GPU process scheduling class %s", cls == 4 ? "HIGH" : "ABOVE_NORMAL");
            return;
        }
    }
    Log::Warn("could not raise GPU scheduling class (run as administrator for best results under heavy GPU load)");
}

std::wstring HrText(HRESULT hr) {
    wchar_t code[16];
    swprintf_s(code, L"0x%08X", unsigned(hr));
    std::wstring text = Win32ErrorText(DWORD(hr));
    return text.empty() ? code : text + L" [" + code + L"]";
}

Status D3DContext::Create(const std::wstring& gdiName) {
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return Status::Error(L"DXGI indisponível: " + HrText(hr));

    for (UINT a = 0; !output; ++a) {
        ComPtr<IDXGIAdapter1> ad;
        if (factory->EnumAdapters1(a, &ad) == DXGI_ERROR_NOT_FOUND) break;
        for (UINT o = 0;; ++o) {
            ComPtr<IDXGIOutput> out;
            if (ad->EnumOutputs(o, &out) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_OUTPUT_DESC desc;
            if (SUCCEEDED(out->GetDesc(&desc)) && gdiName == desc.DeviceName) {
                out.As(&output);
                adapter = ad;
                break;
            }
        }
    }
    if (!output) return Status::Error(L"A GPU que exibe o monitor virtual (" + gdiName + L") não foi encontrada.");

    DXGI_ADAPTER_DESC1 ad;
    adapter->GetDesc1(&ad);
    adapterLuid = ad.AdapterLuid;
    adapterName = ad.Description;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels, ARRAYSIZE(levels),
                           D3D11_SDK_VERSION, &device, nullptr, &context);
    if (FAILED(hr)) return Status::Error(L"Falha ao criar o dispositivo Direct3D na GPU " + adapterName + L": " + HrText(hr));

    // Media Foundation encoders use the device from their own threads.
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(device.As(&mt))) mt->SetMultithreadProtected(TRUE);

    RaiseGpuPriority(device.Get());

    char name[128];
    WideCharToMultiByte(CP_UTF8, 0, adapterName.c_str(), -1, name, sizeof(name), nullptr, nullptr);
    Log::Info("D3D device on '%s' (LUID %08X:%08X)", name, adapterLuid.HighPart, adapterLuid.LowPart);
    return Status::Ok();
}

}  // namespace celmon
