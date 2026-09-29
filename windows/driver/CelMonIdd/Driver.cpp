// CelMonIdd — see Driver.h. Structure follows Microsoft's IddCx sample; monitor lifecycle is driven by the host app.
#include "Driver.h"

#include <cstdarg>
#include <cstdio>

using namespace Microsoft::WRL;
using namespace celmon;

namespace {

constexpr uint32_t kDriverVersion = (0u << 16) | 1u;

struct DeviceContextWrapper {
    DeviceContext* pContext;
    void Cleanup() { delete pContext; pContext = nullptr; }
};

struct MonitorContextWrapper {
    MonitorContext* pContext;
    void Cleanup() { delete pContext; pContext = nullptr; }
};

}  // namespace

WDF_DECLARE_CONTEXT_TYPE(DeviceContextWrapper);
WDF_DECLARE_CONTEXT_TYPE(MonitorContextWrapper);

extern "C" DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD CelMonDeviceAdd;
EVT_WDF_DEVICE_D0_ENTRY CelMonDeviceD0Entry;
EVT_IDD_CX_DEVICE_IO_CONTROL CelMonIoControl;
EVT_IDD_CX_ADAPTER_INIT_FINISHED CelMonAdapterInitFinished;
EVT_IDD_CX_ADAPTER_COMMIT_MODES CelMonAdapterCommitModes;
EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION CelMonParseMonitorDescription;
EVT_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES CelMonMonitorGetDefaultModes;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES CelMonMonitorQueryModes;
EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN CelMonMonitorAssignSwapChain;
EVT_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN CelMonMonitorUnassignSwapChain;

// ---------------------------------------------------------------------------------------------------------- helpers

void celmon::Log(const char* fmt, ...) {
    char buf[512];
    int n = snprintf(buf, sizeof(buf), "[CelMonIdd] ");
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + n, sizeof(buf) - n - 2, fmt, ap);
    va_end(ap);
    strcat_s(buf, "\n");
    OutputDebugStringA(buf);
}

static void FillSignalInfo(DISPLAYCONFIG_VIDEO_SIGNAL_INFO& mode, DWORD width, DWORD height, DWORD vsync, bool monitorMode) {
    mode.totalSize.cx = mode.activeSize.cx = width;
    mode.totalSize.cy = mode.activeSize.cy = height;
    mode.AdditionalSignalInfo.vSyncFreqDivider = monitorMode ? 0 : 1;
    mode.AdditionalSignalInfo.videoStandard = 255;
    mode.vSyncFreq.Numerator = vsync;
    mode.vSyncFreq.Denominator = 1;
    mode.hSyncFreq.Numerator = vsync * height;
    mode.hSyncFreq.Denominator = 1;
    mode.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
    mode.pixelRate = UINT64(vsync) * UINT64(width) * UINT64(height);
}

static IDDCX_MONITOR_MODE MakeMonitorMode(const CELMON_MODE& m, IDDCX_MONITOR_MODE_ORIGIN origin) {
    IDDCX_MONITOR_MODE mode = {};
    mode.Size = sizeof(mode);
    mode.Origin = origin;
    FillSignalInfo(mode.MonitorVideoSignalInfo, m.width, m.height, m.refreshHz, true);
    return mode;
}

static IDDCX_TARGET_MODE MakeTargetMode(const CELMON_MODE& m) {
    IDDCX_TARGET_MODE mode = {};
    mode.Size = sizeof(mode);
    FillSignalInfo(mode.TargetVideoSignalInfo.targetVideoSignalInfo, m.width, m.height, m.refreshHz, false);
    return mode;
}

// Stable container id per phone so Windows keeps its settings across reconnects.
static GUID ContainerIdFor(uint64_t key) {
    GUID g = {0x6c2f0a1e, 0x3b7d, 0x4e55, {0x9a, 0x10, 0, 0, 0, 0, 0, 0}};
    for (int i = 0; i < 6; ++i) g.Data4[2 + i] = uint8_t(key >> (8 * i));
    g.Data2 ^= uint16_t(key >> 48);
    return g;
}

// ---------------------------------------------------------------------------------------------------------- entry

extern "C" BOOL WINAPI DllMain(_In_ HINSTANCE, _In_ UINT, _In_opt_ LPVOID) { return TRUE; }

_Use_decl_annotations_
extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driverObject, PUNICODE_STRING registryPath) {
    WDF_DRIVER_CONFIG config;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    WDF_DRIVER_CONFIG_INIT(&config, CelMonDeviceAdd);
    NTSTATUS status = WdfDriverCreate(driverObject, registryPath, &attributes, &config, WDF_NO_HANDLE);
    Log("DriverEntry status=0x%08X version=%u.%u", status, kDriverVersion >> 16, kDriverVersion & 0xFFFF);
    return status;
}

_Use_decl_annotations_
NTSTATUS CelMonDeviceAdd(WDFDRIVER, PWDFDEVICE_INIT deviceInit) {
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDeviceD0Entry = CelMonDeviceD0Entry;
    WdfDeviceInitSetPnpPowerEventCallbacks(deviceInit, &pnp);

    IDD_CX_CLIENT_CONFIG idd;
    IDD_CX_CLIENT_CONFIG_INIT(&idd);
    // IddCx routes IoDeviceControl to its own queue, so custom IOCTLs must come through this callback.
    idd.EvtIddCxDeviceIoControl = CelMonIoControl;
    idd.EvtIddCxAdapterInitFinished = CelMonAdapterInitFinished;
    idd.EvtIddCxParseMonitorDescription = CelMonParseMonitorDescription;
    idd.EvtIddCxMonitorGetDefaultDescriptionModes = CelMonMonitorGetDefaultModes;
    idd.EvtIddCxMonitorQueryTargetModes = CelMonMonitorQueryModes;
    idd.EvtIddCxAdapterCommitModes = CelMonAdapterCommitModes;
    idd.EvtIddCxMonitorAssignSwapChain = CelMonMonitorAssignSwapChain;
    idd.EvtIddCxMonitorUnassignSwapChain = CelMonMonitorUnassignSwapChain;

    NTSTATUS status = IddCxDeviceInitConfig(deviceInit, &idd);
    if (!NT_SUCCESS(status)) return status;

    WDF_OBJECT_ATTRIBUTES attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, DeviceContextWrapper);
    attr.EvtCleanupCallback = [](WDFOBJECT object) {
        if (auto* w = WdfObjectGet_DeviceContextWrapper(object)) w->Cleanup();
    };

    WDFDEVICE device = nullptr;
    status = WdfDeviceCreate(&deviceInit, &attr, &device);
    if (!NT_SUCCESS(status)) return status;

    status = WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_CELMON_IDD, nullptr);
    if (!NT_SUCCESS(status)) {
        Log("WdfDeviceCreateDeviceInterface failed 0x%08X", status);
        return status;
    }

    status = IddCxDeviceInitialize(device);
    if (!NT_SUCCESS(status)) return status;

    WdfObjectGet_DeviceContextWrapper(device)->pContext = new DeviceContext(device);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS CelMonDeviceD0Entry(WDFDEVICE device, WDF_POWER_DEVICE_STATE) {
    WdfObjectGet_DeviceContextWrapper(device)->pContext->InitAdapter();
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID CelMonIoControl(WDFDEVICE device, WDFREQUEST request, size_t outLen, size_t inLen, ULONG code) {
    WdfObjectGet_DeviceContextWrapper(device)->pContext->HandleIoControl(request, outLen, inLen, code);
}

// ---------------------------------------------------------------------------------------------------------- D3D

HRESULT Direct3DDevice::Init() {
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&DxgiFactory));
    if (FAILED(hr)) return hr;
    hr = DxgiFactory->EnumAdapterByLuid(AdapterLuid, IID_PPV_ARGS(&Adapter));
    if (FAILED(hr)) return hr;
    return D3D11CreateDevice(Adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                             D3D11_SDK_VERSION, &Device, nullptr, &DeviceContext);
}

// ---------------------------------------------------------------------------------------------------------- swap chain

SwapChainProcessor::SwapChainProcessor(IDDCX_SWAPCHAIN swapChain, std::shared_ptr<Direct3DDevice> device, HANDLE newFrameEvent)
    : m_hSwapChain(swapChain), m_Device(std::move(device)), m_hAvailableBufferEvent(newFrameEvent) {
    m_hTerminateEvent.Attach(CreateEvent(nullptr, FALSE, FALSE, nullptr));
    m_hThread.Attach(CreateThread(nullptr, 0, RunThread, this, 0, nullptr));
}

SwapChainProcessor::~SwapChainProcessor() {
    SetEvent(m_hTerminateEvent.Get());
    if (m_hThread.Get()) WaitForSingleObject(m_hThread.Get(), INFINITE);
}

DWORD CALLBACK SwapChainProcessor::RunThread(LPVOID arg) {
    static_cast<SwapChainProcessor*>(arg)->Run();
    return 0;
}

void SwapChainProcessor::Run() {
    DWORD avTask = 0;
    HANDLE avHandle = AvSetMmThreadCharacteristicsW(L"Distribution", &avTask);
    RunCore();
    // Deleting the swap chain tells the OS to provide a new one if it still needs this monitor.
    WdfObjectDelete((WDFOBJECT)m_hSwapChain);
    m_hSwapChain = nullptr;
    if (avHandle) AvRevertMmThreadCharacteristics(avHandle);
}

void SwapChainProcessor::RunCore() {
    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(m_Device->Device.As(&dxgiDevice))) return;

    IDARG_IN_SWAPCHAINSETDEVICE setDevice = {};
    setDevice.pDevice = dxgiDevice.Get();
    HRESULT hr = IddCxSwapChainSetDevice(m_hSwapChain, &setDevice);
    if (FAILED(hr)) {
        Log("IddCxSwapChainSetDevice failed 0x%08X", hr);
        return;
    }

    for (;;) {
        IDARG_OUT_RELEASEANDACQUIREBUFFER buffer = {};
        hr = IddCxSwapChainReleaseAndAcquireBuffer(m_hSwapChain, &buffer);
        if (hr == E_PENDING) {
            HANDLE waits[] = {m_hAvailableBufferEvent, m_hTerminateEvent.Get()};
            DWORD r = WaitForMultipleObjects(ARRAYSIZE(waits), waits, FALSE, 16);
            if (r == WAIT_OBJECT_0 || r == WAIT_TIMEOUT) continue;
            break;  // terminate or unexpected
        }
        if (FAILED(hr)) break;  // swap chain abandoned (mode change, monitor removed...)

        // The host captures this monitor via Desktop Duplication; just hand the surface back promptly.
        if (buffer.MetaData.pSurface) buffer.MetaData.pSurface->Release();
        hr = IddCxSwapChainFinishedProcessingFrame(m_hSwapChain);
        if (FAILED(hr)) break;
    }
}

void MonitorContext::AssignSwapChain(IDDCX_SWAPCHAIN swapChain, LUID renderAdapter, HANDLE newFrameEvent) {
    m_Processor.reset();
    auto device = std::make_shared<Direct3DDevice>(renderAdapter);
    HRESULT hr = device->Init();
    if (FAILED(hr)) {
        Log("monitor %u: D3D init on render adapter failed 0x%08X", m_Record->id, hr);
        WdfObjectDelete(swapChain);  // OS will retry with a new swap chain
        return;
    }
    m_Processor = std::make_unique<SwapChainProcessor>(swapChain, device, newFrameEvent);
}

// ---------------------------------------------------------------------------------------------------------- registry

namespace {
std::mutex g_RegistryLock;
std::vector<std::shared_ptr<MonitorRecord>> g_Registry;
}

void ModeRegistry::Add(const std::shared_ptr<MonitorRecord>& r) {
    std::lock_guard<std::mutex> lock(g_RegistryLock);
    g_Registry.push_back(r);
}

void ModeRegistry::Remove(uint32_t id) {
    std::lock_guard<std::mutex> lock(g_RegistryLock);
    for (auto it = g_Registry.begin(); it != g_Registry.end(); ++it)
        if ((*it)->id == id) { g_Registry.erase(it); return; }
}

std::shared_ptr<MonitorRecord> ModeRegistry::FindByEdid(const void* data, size_t size) {
    if (!data || size != sizeof(Edid)) return nullptr;
    std::lock_guard<std::mutex> lock(g_RegistryLock);
    for (auto& r : g_Registry)
        if (memcmp(r->edid.data(), data, size) == 0) return r;
    return nullptr;
}

// ---------------------------------------------------------------------------------------------------------- device

DeviceContext::DeviceContext(WDFDEVICE device) : m_WdfDevice(device) {
    m_StopWatchdog.Attach(CreateEvent(nullptr, TRUE, FALSE, nullptr));
    m_Watchdog.Attach(CreateThread(nullptr, 0, WatchdogThread, this, 0, nullptr));
}

DeviceContext::~DeviceContext() {
    SetEvent(m_StopWatchdog.Get());
    if (m_Watchdog.Get()) WaitForSingleObject(m_Watchdog.Get(), INFINITE);
}

void DeviceContext::InitAdapter() {
    if (m_Adapter) return;  // D0Entry also runs on resume; the adapter survives that

    IDDCX_ADAPTER_CAPS caps = {};
    caps.Size = sizeof(caps);
    caps.MaxMonitorsSupported = CELMON_MAX_MONITORS;
    caps.EndPointDiagnostics.Size = sizeof(caps.EndPointDiagnostics);
    caps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
    caps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_WIRED_USB;
    caps.EndPointDiagnostics.pEndPointFriendlyName = L"CelMonitor Virtual Display";
    caps.EndPointDiagnostics.pEndPointManufacturerName = L"CelMonitor";
    caps.EndPointDiagnostics.pEndPointModelName = L"CelMonitor USB Display";

    IDDCX_ENDPOINT_VERSION version = {};
    version.Size = sizeof(version);
    version.MajorVer = kDriverVersion >> 16;
    version.MinorVer = kDriverVersion & 0xFFFF;
    caps.EndPointDiagnostics.pFirmwareVersion = &version;
    caps.EndPointDiagnostics.pHardwareVersion = &version;

    WDF_OBJECT_ATTRIBUTES attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, DeviceContextWrapper);

    IDARG_IN_ADAPTER_INIT init = {};
    init.WdfDevice = m_WdfDevice;
    init.pCaps = &caps;
    init.ObjectAttributes = &attr;

    IDARG_OUT_ADAPTER_INIT out = {};
    NTSTATUS status = IddCxAdapterInitAsync(&init, &out);
    Log("IddCxAdapterInitAsync status=0x%08X", status);
    if (NT_SUCCESS(status)) {
        m_Adapter = out.AdapterObject;
        WdfObjectGet_DeviceContextWrapper(out.AdapterObject)->pContext = this;
    }
}

void DeviceContext::OnAdapterInitFinished(NTSTATUS status) {
    Log("adapter init finished status=0x%08X", status);
    m_AdapterReady = NT_SUCCESS(status);
}

void DeviceContext::SetRenderAdapter(LUID luid) {
    if (!m_Adapter) return;
    if (!IDD_IS_FUNCTION_AVAILABLE(IddCxAdapterSetRenderAdapter)) {
        Log("IddCxAdapterSetRenderAdapter not available on this OS");
        return;
    }
    IDARG_IN_ADAPTERSETRENDERADAPTER in = {};
    in.PreferredRenderAdapter = luid;
    IddCxAdapterSetRenderAdapter(m_Adapter, &in);
    Log("preferred render adapter set to %08X:%08X", luid.HighPart, luid.LowPart);
}

NTSTATUS DeviceContext::AddMonitor(const CELMON_ADD_MONITOR_IN& in, CELMON_ADD_MONITOR_OUT& out) {
    // ---- validate everything coming from user mode
    if (in.apiVersion != CELMON_DRIVER_API_VERSION) return STATUS_REVISION_MISMATCH;
    if (in.modeCount == 0 || in.modeCount > CELMON_MAX_MODES || in.preferredMode >= in.modeCount)
        return STATUS_INVALID_PARAMETER;
    for (uint32_t i = 0; i < in.modeCount; ++i) {
        const auto& m = in.modes[i];
        if (m.width < 320 || m.width > 7680 || m.height < 320 || m.height > 7680 || m.refreshHz < 1 || m.refreshHz > 240)
            return STATUS_INVALID_PARAMETER;
    }
    char name[14] = {};
    for (int i = 0; i < 13 && in.deviceName[i]; ++i) {
        char c = in.deviceName[i];
        name[i] = (c >= 0x20 && c < 0x7F) ? c : '?';
    }
    if (!m_AdapterReady) return STATUS_DEVICE_NOT_READY;

    auto rec = std::make_shared<MonitorRecord>();
    {
        std::lock_guard<std::mutex> lock(m_Lock);
        if (m_Monitors.size() >= CELMON_MAX_MONITORS) return STATUS_TOO_MANY_NODES;
        for (auto& m : m_Monitors)
            if (m->deviceKey == in.deviceKey) return STATUS_OBJECT_NAME_COLLISION;
        uint32_t used = 0;
        for (auto& m : m_Monitors) used |= 1u << m->connectorIndex;
        while (used & (1u << rec->connectorIndex)) ++rec->connectorIndex;
        rec->id = m_NextId++;
    }
    rec->deviceKey = in.deviceKey;
    rec->modes.assign(in.modes, in.modes + in.modeCount);
    rec->preferred = in.preferredMode;
    rec->edid = BuildEdid(in.deviceKey, name[0] ? name : "CelMonitor", in.modes[in.preferredMode]);
    ModeRegistry::Add(rec);

    WDF_OBJECT_ATTRIBUTES attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, MonitorContextWrapper);
    attr.EvtCleanupCallback = [](WDFOBJECT object) {
        if (auto* w = WdfObjectGet_MonitorContextWrapper(object)) w->Cleanup();
    };

    IDDCX_MONITOR_INFO info = {};
    info.Size = sizeof(info);
    info.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_WIRED;
    info.ConnectorIndex = rec->connectorIndex;
    info.MonitorDescription.Size = sizeof(info.MonitorDescription);
    info.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
    info.MonitorDescription.DataSize = sizeof(Edid);
    info.MonitorDescription.pData = rec->edid.data();
    info.MonitorContainerId = ContainerIdFor(in.deviceKey);

    IDARG_IN_MONITORCREATE create = {};
    create.ObjectAttributes = &attr;
    create.pMonitorInfo = &info;
    IDARG_OUT_MONITORCREATE created = {};
    NTSTATUS status = IddCxMonitorCreate(m_Adapter, &create, &created);
    if (!NT_SUCCESS(status)) {
        Log("IddCxMonitorCreate failed 0x%08X", status);
        ModeRegistry::Remove(rec->id);
        return status;
    }
    rec->monitor = created.MonitorObject;
    WdfObjectGet_MonitorContextWrapper(created.MonitorObject)->pContext = new MonitorContext(rec);

    IDARG_OUT_MONITORARRIVAL arrival = {};
    status = IddCxMonitorArrival(created.MonitorObject, &arrival);
    if (!NT_SUCCESS(status)) {
        Log("IddCxMonitorArrival failed 0x%08X", status);
        WdfObjectDelete(created.MonitorObject);
        ModeRegistry::Remove(rec->id);
        return status;
    }
    {
        std::lock_guard<std::mutex> lock(m_Lock);
        m_Monitors.push_back(rec);
    }
    m_LastPing = GetTickCount64();
    out.monitorId = rec->id;
    out.targetId = arrival.OsTargetId;
    out.adapterLuid = arrival.OsAdapterLuid;
    Log("monitor %u added: connector=%u target=%u modes=%u preferred=%ux%u@%u", rec->id, rec->connectorIndex,
        arrival.OsTargetId, in.modeCount, in.modes[in.preferredMode].width, in.modes[in.preferredMode].height,
        in.modes[in.preferredMode].refreshHz);
    return STATUS_SUCCESS;
}

NTSTATUS DeviceContext::RemoveMonitor(uint32_t id) {
    std::shared_ptr<MonitorRecord> rec;
    {
        std::lock_guard<std::mutex> lock(m_Lock);
        for (auto it = m_Monitors.begin(); it != m_Monitors.end(); ++it) {
            if ((*it)->id == id) { rec = *it; m_Monitors.erase(it); break; }
        }
    }
    if (!rec) return STATUS_NOT_FOUND;
    NTSTATUS status = IddCxMonitorDeparture(rec->monitor);
    ModeRegistry::Remove(id);
    Log("monitor %u removed status=0x%08X", id, status);
    return status;
}

void DeviceContext::RemoveAllMonitors(const char* why) {
    std::vector<uint32_t> ids;
    {
        std::lock_guard<std::mutex> lock(m_Lock);
        for (auto& m : m_Monitors) ids.push_back(m->id);
    }
    if (!ids.empty()) Log("removing all monitors: %s", why);
    for (uint32_t id : ids) RemoveMonitor(id);
}

DWORD CALLBACK DeviceContext::WatchdogThread(LPVOID arg) {
    static_cast<DeviceContext*>(arg)->Watchdog();
    return 0;
}

void DeviceContext::Watchdog() {
    while (WaitForSingleObject(m_StopWatchdog.Get(), 1000) == WAIT_TIMEOUT) {
        bool any;
        {
            std::lock_guard<std::mutex> lock(m_Lock);
            any = !m_Monitors.empty();
        }
        if (any && GetTickCount64() - m_LastPing > CELMON_WATCHDOG_TIMEOUT_MS) RemoveAllMonitors("host stopped pinging");
    }
}

void DeviceContext::HandleIoControl(WDFREQUEST request, size_t outLen, size_t inLen, ULONG code) {
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    size_t written = 0;
    void* inBuf = nullptr;
    void* outBuf = nullptr;
    m_LastPing = GetTickCount64();

    switch (code) {
    case IOCTL_CELMON_GET_INFO:
        status = WdfRequestRetrieveOutputBuffer(request, sizeof(CELMON_INFO), &outBuf, nullptr);
        if (NT_SUCCESS(status)) {
            auto* info = static_cast<CELMON_INFO*>(outBuf);
            info->apiVersion = CELMON_DRIVER_API_VERSION;
            info->driverVersion = kDriverVersion;
            info->maxMonitors = CELMON_MAX_MONITORS;
            std::lock_guard<std::mutex> lock(m_Lock);
            info->activeMonitors = uint32_t(m_Monitors.size());
            written = sizeof(CELMON_INFO);
        }
        break;

    case IOCTL_CELMON_ADD_MONITOR:
        status = WdfRequestRetrieveInputBuffer(request, sizeof(CELMON_ADD_MONITOR_IN), &inBuf, nullptr);
        if (NT_SUCCESS(status)) status = WdfRequestRetrieveOutputBuffer(request, sizeof(CELMON_ADD_MONITOR_OUT), &outBuf, nullptr);
        if (NT_SUCCESS(status)) {
            // Copy first: METHOD_BUFFERED shares one system buffer for input and output.
            CELMON_ADD_MONITOR_IN in = *static_cast<const CELMON_ADD_MONITOR_IN*>(inBuf);
            CELMON_ADD_MONITOR_OUT out = {};
            status = AddMonitor(in, out);
            if (NT_SUCCESS(status)) {
                *static_cast<CELMON_ADD_MONITOR_OUT*>(outBuf) = out;
                written = sizeof(out);
            }
        }
        break;

    case IOCTL_CELMON_REMOVE_MONITOR:
        status = WdfRequestRetrieveInputBuffer(request, sizeof(CELMON_REMOVE_MONITOR_IN), &inBuf, nullptr);
        if (NT_SUCCESS(status)) status = RemoveMonitor(static_cast<const CELMON_REMOVE_MONITOR_IN*>(inBuf)->monitorId);
        break;

    case IOCTL_CELMON_PING:
        status = STATUS_SUCCESS;
        break;

    case IOCTL_CELMON_SET_RENDER_ADAPTER:
        status = WdfRequestRetrieveInputBuffer(request, sizeof(CELMON_SET_RENDER_ADAPTER_IN), &inBuf, nullptr);
        if (NT_SUCCESS(status)) SetRenderAdapter(static_cast<const CELMON_SET_RENDER_ADAPTER_IN*>(inBuf)->renderAdapterLuid);
        break;
    }
    UNREFERENCED_PARAMETER(outLen);
    UNREFERENCED_PARAMETER(inLen);
    WdfRequestCompleteWithInformation(request, status, written);
}

// ---------------------------------------------------------------------------------------------------------- IddCx DDIs

_Use_decl_annotations_
NTSTATUS CelMonAdapterInitFinished(IDDCX_ADAPTER adapter, const IDARG_IN_ADAPTER_INIT_FINISHED* in) {
    WdfObjectGet_DeviceContextWrapper(adapter)->pContext->OnAdapterInitFinished(in->AdapterInitStatus);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS CelMonAdapterCommitModes(IDDCX_ADAPTER, const IDARG_IN_COMMITMODES*) {
    return STATUS_SUCCESS;  // nothing to program: the "scan-out" is the host app capturing the monitor
}

_Use_decl_annotations_
NTSTATUS CelMonParseMonitorDescription(const IDARG_IN_PARSEMONITORDESCRIPTION* in, IDARG_OUT_PARSEMONITORDESCRIPTION* out) {
    auto rec = ModeRegistry::FindByEdid(in->MonitorDescription.pData, in->MonitorDescription.DataSize);
    if (!rec) return STATUS_INVALID_PARAMETER;
    out->MonitorModeBufferOutputCount = UINT(rec->modes.size());
    if (in->MonitorModeBufferInputCount < rec->modes.size())
        return in->MonitorModeBufferInputCount > 0 ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;
    for (size_t i = 0; i < rec->modes.size(); ++i)
        in->pMonitorModes[i] = MakeMonitorMode(rec->modes[i], IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR);
    out->PreferredMonitorModeIdx = rec->preferred;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS CelMonMonitorGetDefaultModes(IDDCX_MONITOR, const IDARG_IN_GETDEFAULTDESCRIPTIONMODES*, IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* out) {
    // Only called for EDID-less monitors; ours always have an EDID.
    out->DefaultMonitorModeBufferOutputCount = 0;
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS CelMonMonitorQueryModes(IDDCX_MONITOR monitor, const IDARG_IN_QUERYTARGETMODES* in, IDARG_OUT_QUERYTARGETMODES* out) {
    auto* ctx = WdfObjectGet_MonitorContextWrapper(monitor)->pContext;
    if (!ctx) return STATUS_INVALID_PARAMETER;
    const auto& modes = ctx->Record().modes;
    out->TargetModeBufferOutputCount = UINT(modes.size());
    if (in->TargetModeBufferInputCount >= modes.size())
        for (size_t i = 0; i < modes.size(); ++i) in->pTargetModes[i] = MakeTargetMode(modes[i]);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS CelMonMonitorAssignSwapChain(IDDCX_MONITOR monitor, const IDARG_IN_SETSWAPCHAIN* in) {
    WdfObjectGet_MonitorContextWrapper(monitor)->pContext->AssignSwapChain(in->hSwapChain, in->RenderAdapterLuid,
                                                                           in->hNextSurfaceAvailable);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS CelMonMonitorUnassignSwapChain(IDDCX_MONITOR monitor) {
    WdfObjectGet_MonitorContextWrapper(monitor)->pContext->UnassignSwapChain();
    return STATUS_SUCCESS;
}
