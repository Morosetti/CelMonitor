// Interface between the CelMonIdd driver and the CelMonitor host app (IOCTLs over a device interface).
// Shared by windows/driver and windows/host. Plain C layout, fixed-size, versioned.
#pragma once

#include <windows.h>
#include <winioctl.h>
#include <stdint.h>

// {47ED6B76-8D82-44AF-9839-0E905A415669}
// Defined inline (not DEFINE_GUID) so no translation unit needs initguid.h.
static const GUID GUID_DEVINTERFACE_CELMON_IDD = {0x47ed6b76, 0x8d82, 0x44af, {0x98, 0x39, 0x0e, 0x90, 0x5a, 0x41, 0x56, 0x69}};

#define CELMON_DRIVER_API_VERSION 1u
#define CELMON_MAX_MONITORS 4u
#define CELMON_MAX_MODES 32u
// Monitors are removed automatically if the host stops pinging for this long (host crashed, killed, ...).
#define CELMON_WATCHDOG_TIMEOUT_MS 10000u

#define CELMON_IOCTL(n) CTL_CODE(FILE_DEVICE_UNKNOWN, 0x900 + (n), METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define IOCTL_CELMON_GET_INFO       CELMON_IOCTL(0)
#define IOCTL_CELMON_ADD_MONITOR    CELMON_IOCTL(1)
#define IOCTL_CELMON_REMOVE_MONITOR CELMON_IOCTL(2)
#define IOCTL_CELMON_PING           CELMON_IOCTL(3)
#define IOCTL_CELMON_SET_RENDER_ADAPTER CELMON_IOCTL(4)

#pragma pack(push, 4)

typedef struct CELMON_INFO {
    uint32_t apiVersion;
    uint32_t driverVersion;  // major << 16 | minor
    uint32_t maxMonitors;
    uint32_t activeMonitors;
} CELMON_INFO;

typedef struct CELMON_MODE {
    uint32_t width;
    uint32_t height;
    uint32_t refreshHz;
} CELMON_MODE;

typedef struct CELMON_ADD_MONITOR_IN {
    uint32_t apiVersion;           // CELMON_DRIVER_API_VERSION
    uint32_t modeCount;            // 1..CELMON_MAX_MODES
    uint32_t preferredMode;        // index into modes
    uint32_t reserved;
    uint64_t deviceKey;            // stable per phone: EDID serial + container id, so Windows remembers its layout
    char deviceName[14];           // shown as the monitor name (EDID, ASCII, <= 13 chars)
    char reserved2[2];
    CELMON_MODE modes[CELMON_MAX_MODES];
} CELMON_ADD_MONITOR_IN;

typedef struct CELMON_ADD_MONITOR_OUT {
    uint32_t monitorId;            // handle for REMOVE_MONITOR
    uint32_t targetId;             // DISPLAYCONFIG target id of the new monitor
    LUID adapterLuid;              // OS adapter LUID of the indirect adapter (match in QueryDisplayConfig)
} CELMON_ADD_MONITOR_OUT;

typedef struct CELMON_REMOVE_MONITOR_IN {
    uint32_t monitorId;
} CELMON_REMOVE_MONITOR_IN;

typedef struct CELMON_SET_RENDER_ADAPTER_IN {
    LUID renderAdapterLuid;        // GPU that should render (and whose encoder we use)
} CELMON_SET_RENDER_ADAPTER_IN;

#pragma pack(pop)
