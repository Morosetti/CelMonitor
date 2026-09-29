// CelMonIdd — Indirect Display Driver (UMDF 2 + IddCx) that creates the phone's virtual monitor.
// Monitors are created/removed on demand by the host app through IOCTLs (see shared/celmon_driver_api.h).
#pragma once

#define NOMINMAX
#include <windows.h>
#include <bugcodes.h>
#include <wudfwdm.h>
#include <wdf.h>
#include <iddcx.h>

#include <dxgi1_5.h>
#include <d3d11_2.h>
#include <avrt.h>
#include <wrl.h>

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include "../../shared/celmon_driver_api.h"

namespace celmon {

void Log(const char* fmt, ...);

using Edid = std::array<uint8_t, 128>;

Edid BuildEdid(uint64_t deviceKey, const char* name, const CELMON_MODE& preferred);

struct Direct3DDevice {
    explicit Direct3DDevice(LUID luid) : AdapterLuid(luid) {}
    HRESULT Init();

    LUID AdapterLuid;
    Microsoft::WRL::ComPtr<IDXGIFactory5> DxgiFactory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> Adapter;
    Microsoft::WRL::ComPtr<ID3D11Device> Device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> DeviceContext;
};

// Drains the swap chain IddCx gives us. The host app captures the monitor with Desktop Duplication, so the driver
// only has to keep frames flowing (acquire + release) for the OS compositor to keep presenting.
class SwapChainProcessor {
public:
    SwapChainProcessor(IDDCX_SWAPCHAIN swapChain, std::shared_ptr<Direct3DDevice> device, HANDLE newFrameEvent);
    ~SwapChainProcessor();

private:
    static DWORD CALLBACK RunThread(LPVOID arg);
    void Run();
    void RunCore();

    IDDCX_SWAPCHAIN m_hSwapChain;
    std::shared_ptr<Direct3DDevice> m_Device;
    HANDLE m_hAvailableBufferEvent;
    Microsoft::WRL::Wrappers::Event m_hTerminateEvent;
    Microsoft::WRL::Wrappers::HandleT<Microsoft::WRL::Wrappers::HandleTraits::HANDLENullTraits> m_hThread;
};

// One virtual monitor (one connected phone).
struct MonitorRecord {
    uint32_t id = 0;
    uint32_t connectorIndex = 0;
    uint64_t deviceKey = 0;
    IDDCX_MONITOR monitor = nullptr;
    std::vector<CELMON_MODE> modes;
    uint32_t preferred = 0;
    Edid edid{};
};

class MonitorContext {
public:
    explicit MonitorContext(std::shared_ptr<MonitorRecord> record) : m_Record(std::move(record)) {}
    ~MonitorContext() { m_Processor.reset(); }

    void AssignSwapChain(IDDCX_SWAPCHAIN swapChain, LUID renderAdapter, HANDLE newFrameEvent);
    void UnassignSwapChain() { m_Processor.reset(); }
    const MonitorRecord& Record() const { return *m_Record; }

private:
    std::shared_ptr<MonitorRecord> m_Record;
    std::unique_ptr<SwapChainProcessor> m_Processor;
};

class DeviceContext {
public:
    explicit DeviceContext(WDFDEVICE device);
    ~DeviceContext();

    void InitAdapter();
    void OnAdapterInitFinished(NTSTATUS status);
    void HandleIoControl(WDFREQUEST request, size_t outLen, size_t inLen, ULONG code);

private:
    NTSTATUS AddMonitor(const CELMON_ADD_MONITOR_IN& in, CELMON_ADD_MONITOR_OUT& out);
    NTSTATUS RemoveMonitor(uint32_t id);
    void RemoveAllMonitors(const char* why);
    void SetRenderAdapter(LUID luid);
    static DWORD CALLBACK WatchdogThread(LPVOID arg);
    void Watchdog();

    WDFDEVICE m_WdfDevice;
    IDDCX_ADAPTER m_Adapter = nullptr;
    std::atomic<bool> m_AdapterReady{false};

    std::mutex m_Lock;  // guards m_Monitors and m_NextId; never held across IddCx calls
    std::vector<std::shared_ptr<MonitorRecord>> m_Monitors;
    uint32_t m_NextId = 1;

    std::atomic<ULONGLONG> m_LastPing{0};
    Microsoft::WRL::Wrappers::Event m_StopWatchdog;
    Microsoft::WRL::Wrappers::HandleT<Microsoft::WRL::Wrappers::HandleTraits::HANDLENullTraits> m_Watchdog;
};

// EDID → modes lookup for EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION, which has no monitor object.
class ModeRegistry {
public:
    static void Add(const std::shared_ptr<MonitorRecord>& r);
    static void Remove(uint32_t id);
    static std::shared_ptr<MonitorRecord> FindByEdid(const void* data, size_t size);
};

}  // namespace celmon
