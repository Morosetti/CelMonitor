#include "AoaTransport.h"

#include <cfgmgr32.h>
#include <setupapi.h>

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <fstream>
#include <mutex>

#include "../util/Log.h"

#pragma comment(lib, "winusb.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")

namespace celmon {

namespace {

// AOA 2.0 control requests (https://source.android.com/docs/core/interaction/accessories/aoa2)
constexpr UCHAR kAoaGetProtocol = 51;
constexpr UCHAR kAoaSendString = 52;
constexpr UCHAR kAoaStart = 53;
// Must match android/app/src/main/res/xml/accessory_filter.xml
const char* const kAoaStrings[] = {
    "CelMonitor",                                   // 0 manufacturer
    "CelMonitor Display",                           // 1 model
    "Monitor USB para Windows",                     // 2 description
    "1",                                            // 3 version
    "https://github.com/Morosetti/CelMonitor",      // 4 URI (shown if the app is missing)
    "CELMONITOR",                                   // 5 serial
};

std::string SerialFromInstance(const std::wstring& instance) {
    size_t p = instance.find_last_of(L'\\');
    std::wstring tail = p == std::wstring::npos ? instance : instance.substr(p + 1);
    if (tail.find(L'&') != std::wstring::npos) return {};  // generated id: the device has no USB serial
    std::string s;
    for (wchar_t c : tail) s += char(std::towlower(c));
    return s;
}

// Overlapped-I/O WinUSB connection: reads and writes run concurrently on different pipes, and Close() aborts both.
class WinUsbConnection : public IConnection {
public:
    WinUsbConnection(HANDLE file, WINUSB_INTERFACE_HANDLE usb, UCHAR in, UCHAR out, std::wstring desc,
                     const std::vector<uint8_t>& leftover = {})
        : file_(file), usb_(usb), in_(in), out_(out), desc_(std::move(desc)) {
        readEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        writeEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        len_ = std::min(leftover.size(), sizeof(buffer_));  // bytes received right after the sync marker
        memcpy(buffer_, leftover.data(), len_);
    }
    ~WinUsbConnection() override {
        Close();
        WinUsb_Free(usb_);
        CloseHandle(file_);
        CloseHandle(readEvent_);
        CloseHandle(writeEvent_);
    }

    bool ReadExact(void* buf, size_t n) override {
        auto* dst = static_cast<uint8_t*>(buf);
        while (n > 0) {
            if (pos_ == len_) {
                // Large requests: a short packet (or ZLP) completes them early, so latency is unaffected.
                ULONG got = 0;
                if (!Transfer(true, buffer_, sizeof(buffer_), &got)) return false;
                pos_ = 0;
                len_ = got;
                continue;
            }
            size_t k = std::min(n, len_ - pos_);
            memcpy(dst, buffer_ + pos_, k);
            pos_ += k;
            dst += k;
            n -= k;
        }
        return true;
    }

    // Every transfer is <= 16000 bytes and never a multiple of the packet size, so it always ends with a short packet
    // and fits the phone's single 16 KB read request (f_accessory BULK_BUFFER_SIZE). Larger transfers made the phone's
    // read fail with EINVAL (measured on a Mi Max 3), and zero-length packets are reported as end-of-file by older
    // kernels, so we never rely on them.
    bool WriteAll(const void* buf, size_t n) override {
        auto* src = static_cast<const uint8_t*>(buf);
        while (n > 0) {
            size_t chunk = std::min<size_t>(n, maxTransfer_);
            if (chunk % maxPacket_ == 0) chunk -= 1;  // leaves at least one byte for a following short transfer
            ULONG sent = 0;
            if (!Transfer(false, const_cast<uint8_t*>(src), ULONG(chunk), &sent) || sent == 0) return false;
            src += sent;
            n -= sent;
        }
        return true;
    }

    void SetMaxPacket(USHORT size) { maxPacket_ = size ? size : 512; }
    void SetMaxTransfer(uint32_t bytes) { maxTransfer_ = std::max<uint32_t>(bytes, 2u * maxPacket_); }

    void Close() override {
        if (closed_.exchange(true)) return;
        WinUsb_AbortPipe(usb_, in_);   // unblocks a pending read
        WinUsb_AbortPipe(usb_, out_);  // and write
    }

    std::wstring Description() const override { return desc_; }

private:
    bool Transfer(bool read, uint8_t* p, ULONG n, ULONG* done) {
        if (closed_) return false;
        OVERLAPPED ov = {};
        ov.hEvent = read ? readEvent_ : writeEvent_;
        ResetEvent(ov.hEvent);
        BOOL ok = read ? WinUsb_ReadPipe(usb_, in_, p, n, nullptr, &ov) : WinUsb_WritePipe(usb_, out_, p, n, nullptr, &ov);
        if (!ok && GetLastError() != ERROR_IO_PENDING) return false;
        if (!WinUsb_GetOverlappedResult(usb_, &ov, done, TRUE)) return false;
        return !closed_;
    }

    HANDLE file_;
    WINUSB_INTERFACE_HANDLE usb_;
    UCHAR in_, out_;
    std::wstring desc_;
    HANDLE readEvent_, writeEvent_;
    std::atomic<bool> closed_{false};
    USHORT maxPacket_ = 512;
    uint32_t maxTransfer_ = 16000;
    uint8_t buffer_[64 * 1024];
    size_t pos_ = 0, len_ = 0;
};

struct OpenedUsb {
    HANDLE file = INVALID_HANDLE_VALUE;
    WINUSB_INTERFACE_HANDLE usb = nullptr;
    void Reset() {
        if (usb) WinUsb_Free(usb);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        usb = nullptr;
        file = INVALID_HANDLE_VALUE;
    }
};

Status OpenUsb(const std::wstring& path, OpenedUsb& o) {
    o.file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (o.file == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        return Status::Error(L"Não foi possível abrir o dispositivo USB: " + Win32ErrorText(e));
    }
    if (!WinUsb_Initialize(o.file, &o.usb)) {
        DWORD e = GetLastError();
        o.Reset();
        return Status::Error(L"WinUSB recusou o dispositivo: " + Win32ErrorText(e));
    }
    return Status::Ok();
}

// ---- Stream synchronization (see docs/PROTOCOLO.md, "Sincronização do USB acessório").
// The accessory channel outlives sessions: bytes of a previous attempt (a stale HELLO, the tail of a video frame)
// can still be queued when a new session starts. Both sides discard everything until they see the other's
// marker for a fresh random nonce. Markers are 16 bytes: an 8-byte ASCII tag + little-endian u64 nonce.
constexpr char kSyncReq[] = "CELMSYNC";   // PC -> phone, repeated until answered
constexpr char kSyncAck[] = "CELMSACK";   // phone -> PC, echoes the nonce
constexpr char kSyncDone[] = "CELMSDON";  // PC -> phone, protocol starts right after it
constexpr size_t kMarkerSize = 16;

std::vector<uint8_t> Marker(const char* tag, uint64_t nonce) {
    std::vector<uint8_t> m(kMarkerSize);
    memcpy(m.data(), tag, 8);
    for (int i = 0; i < 8; ++i) m[8 + i] = uint8_t(nonce >> (8 * i));
    return m;
}

bool PipeTransfer(WINUSB_INTERFACE_HANDLE usb, UCHAR pipe, bool read, uint8_t* p, ULONG n, ULONG* done) {
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    BOOL ok = read ? WinUsb_ReadPipe(usb, pipe, p, n, nullptr, &ov) : WinUsb_WritePipe(usb, pipe, p, n, nullptr, &ov);
    if (ok || GetLastError() == ERROR_IO_PENDING) ok = WinUsb_GetOverlappedResult(usb, &ov, done, TRUE);
    CloseHandle(ov.hEvent);
    return ok != FALSE;
}

void SetTimeout(WINUSB_INTERFACE_HANDLE usb, UCHAR pipe, ULONG ms) {
    WinUsb_SetPipePolicy(usb, pipe, PIPE_TRANSFER_TIMEOUT, sizeof(ms), &ms);
}

Status Synchronize(WINUSB_INTERFACE_HANDLE usb, UCHAR in, UCHAR out, std::vector<uint8_t>& leftover) {
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    uint64_t nonce = (uint64_t(qpc.QuadPart) * 0x9E3779B97F4A7C15ull) ^ GetTickCount64() ^ (uint64_t(GetCurrentProcessId()) << 32);
    auto req = Marker(kSyncReq, nonce);
    auto ack = Marker(kSyncAck, nonce);
    // Short timeouts only while synchronizing: the app may not be reading yet (writes would block forever).
    SetTimeout(usb, in, 250);
    SetTimeout(usb, out, 250);
    std::vector<uint8_t> seen;
    std::vector<uint8_t> chunk(64 * 1024);
    Status result = Status::Error(L"O app no celular não respondeu pela conexão USB direta (está aberto e desbloqueado?).");
    for (ULONGLONG deadline = GetTickCount64() + 10000; GetTickCount64() < deadline;) {
        ULONG n = 0;
        PipeTransfer(usb, out, false, req.data(), ULONG(req.size()), &n);  // a timeout here is fine: retried
        n = 0;
        if (!PipeTransfer(usb, in, true, chunk.data(), ULONG(chunk.size()), &n) || n == 0) continue;
        seen.insert(seen.end(), chunk.begin(), chunk.begin() + n);
        auto it = std::search(seen.begin(), seen.end(), ack.begin(), ack.end());
        if (it != seen.end()) {
            leftover.assign(it + kMarkerSize, seen.end());
            auto done = Marker(kSyncDone, nonce);
            if (PipeTransfer(usb, out, false, done.data(), ULONG(done.size()), &n)) result = Status::Ok();
            break;
        }
        if (seen.size() > 256 * 1024) seen.erase(seen.begin(), seen.end() - (kMarkerSize - 1));  // stale data
    }
    SetTimeout(usb, in, 0);   // back to "no timeout" for the session: a timed-out write could cut a frame in half
    SetTimeout(usb, out, 0);
    return result;
}

}  // namespace

std::vector<UsbInterfaceInfo> AoaTransport::Enumerate(const GUID& interfaceGuid) {
    std::vector<UsbInterfaceInfo> list;
    HDEVINFO set = SetupDiGetClassDevsW(&interfaceGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return list;
    SP_DEVICE_INTERFACE_DATA ifd = {sizeof(ifd)};
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &interfaceGuid, i, &ifd); ++i) {
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &needed, nullptr);
        std::vector<uint8_t> buf(needed);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        detail->cbSize = sizeof(*detail);
        SP_DEVINFO_DATA dev = {sizeof(dev)};
        if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, detail, needed, nullptr, &dev)) continue;
        UsbInterfaceInfo info;
        info.path = detail->DevicePath;
        wchar_t id[MAX_DEVICE_ID_LEN] = {};
        CM_Get_Device_IDW(dev.DevInst, id, MAX_DEVICE_ID_LEN, 0);
        info.instance = id;
        unsigned vid = 0, pid = 0;
        if (const wchar_t* v = wcsstr(id, L"VID_")) swscanf_s(v, L"VID_%4x", &vid);
        if (const wchar_t* p = wcsstr(id, L"PID_")) swscanf_s(p, L"PID_%4x", &pid);
        info.vid = uint16_t(vid);
        info.pid = uint16_t(pid);
        // Interfaces of composite devices (MI_xx) carry the serial on their parent.
        std::wstring serialSource = id;
        if (wcsstr(id, L"&MI_")) {
            DEVINST parent;
            wchar_t pid2[MAX_DEVICE_ID_LEN] = {};
            if (CM_Get_Parent(&parent, dev.DevInst, 0) == CR_SUCCESS && CM_Get_Device_IDW(parent, pid2, MAX_DEVICE_ID_LEN, 0) == CR_SUCCESS)
                serialSource = pid2;
        }
        info.serial = SerialFromInstance(serialSource);
        list.push_back(info);
    }
    SetupDiDestroyDeviceInfoList(set);
    return list;
}

bool AoaTransport::DriverInstalled() {
    // Published driver packages live in %WINDIR%\INF\oem*.inf; ours references CelMonAoa.cat.
    wchar_t dir[MAX_PATH];
    GetWindowsDirectoryW(dir, MAX_PATH);
    std::wstring pattern = std::wstring(dir) + L"\\INF\\oem*.inf";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    do {
        std::ifstream f(std::wstring(dir) + L"\\INF\\" + fd.cFileName, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        // INFs may be UTF-16; compare on an ASCII-folded copy.
        std::string ascii;
        for (char c : text) if (c) ascii += char(std::tolower(static_cast<unsigned char>(c)));
        if (ascii.find("celmonaoa.cat") != std::string::npos) found = true;
    } while (!found && FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

Status AoaTransport::SwitchToAccessory(const UsbInterfaceInfo& adbInterface, int* protocolOut) {
    OpenedUsb o;
    Status s = OpenUsb(adbInterface.path, o);
    if (!s.ok) return Status::Error(s.message + L" (o servidor adb pode estar usando a interface)");

    WINUSB_SETUP_PACKET setup = {};
    setup.RequestType = 0xC0;  // device-to-host | vendor | device
    setup.Request = kAoaGetProtocol;
    setup.Length = 2;
    uint8_t version[2] = {};
    ULONG got = 0;
    if (!WinUsb_ControlTransfer(o.usb, setup, version, 2, &got, nullptr) || got != 2) {
        DWORD e = GetLastError();
        o.Reset();
        return Status::Error(L"O celular não respondeu ao pedido AOA: " + Win32ErrorText(e));
    }
    int protocol = version[0] | (version[1] << 8);
    if (protocolOut) *protocolOut = protocol;
    if (protocol < 1) {
        o.Reset();
        return Status::Error(L"Este celular não suporta o modo acessório USB (AOA).");
    }
    for (USHORT i = 0; i < ARRAYSIZE(kAoaStrings); ++i) {
        // WinUSB rejects read-only buffers (ERROR_NOACCESS) even for OUT transfers: copy the literal first.
        std::vector<uint8_t> str(kAoaStrings[i], kAoaStrings[i] + strlen(kAoaStrings[i]) + 1);
        WINUSB_SETUP_PACKET sp = {};
        sp.RequestType = 0x40;  // host-to-device | vendor | device
        sp.Request = kAoaSendString;
        sp.Index = i;
        sp.Length = USHORT(str.size());
        if (!WinUsb_ControlTransfer(o.usb, sp, str.data(), sp.Length, &got, nullptr)) {
            DWORD e = GetLastError();
            o.Reset();
            return Status::Error(L"Falha ao identificar o PC para o celular (AOA): " + Win32ErrorText(e));
        }
    }
    WINUSB_SETUP_PACKET start = {};
    start.RequestType = 0x40;
    start.Request = kAoaStart;
    // The phone drops off the bus right after this request; an error here is expected on some devices.
    WinUsb_ControlTransfer(o.usb, start, nullptr, 0, &got, nullptr);
    o.Reset();
    Log::Info("AOA v%d switch sent to %s", protocol, adbInterface.serial.c_str());
    return Status::Ok();
}

Status AoaTransport::Open(const std::string& serial, std::unique_ptr<IConnection>& out, uint32_t maxTransfer) {
    auto list = Enumerate(GUID_DEVINTERFACE_CELMON_AOA);
    const UsbInterfaceInfo* pick = nullptr;
    for (auto& i : list)
        if (serial.empty() || i.serial.empty() || i.serial == serial) { pick = &i; break; }
    if (!pick) return Status::Error(L"Nenhum celular em modo acessório USB.");

    OpenedUsb o;
    Status s = OpenUsb(pick->path, o);
    if (!s.ok) return s;
    USB_INTERFACE_DESCRIPTOR iface = {};
    if (!WinUsb_QueryInterfaceSettings(o.usb, 0, &iface)) {
        DWORD e = GetLastError();
        o.Reset();
        return Status::Error(L"Falha ao ler a interface USB do acessório: " + Win32ErrorText(e));
    }
    UCHAR in = 0, outPipe = 0;
    USHORT outMaxPacket = 512;
    for (UCHAR i = 0; i < iface.bNumEndpoints; ++i) {
        WINUSB_PIPE_INFORMATION pipe;
        if (!WinUsb_QueryPipe(o.usb, 0, i, &pipe) || pipe.PipeType != UsbdPipeTypeBulk) continue;
        if (USB_ENDPOINT_DIRECTION_IN(pipe.PipeId)) {
            in = pipe.PipeId;
        } else {
            outPipe = pipe.PipeId;
            outMaxPacket = pipe.MaximumPacketSize;
        }
    }
    if (!in || !outPipe) {
        o.Reset();
        return Status::Error(L"O acessório USB não tem os endpoints bulk esperados.");
    }
    // No automatic zero-length packets: WriteAll() shapes every transfer to end with a short packet instead.
    UCHAR off = FALSE;
    WinUsb_SetPipePolicy(o.usb, outPipe, SHORT_PACKET_TERMINATE, sizeof(off), &off);
    WinUsb_FlushPipe(o.usb, in);  // drop anything left from a previous session

    std::vector<uint8_t> leftover;
    s = Synchronize(o.usb, in, outPipe, leftover);
    if (!s.ok) {
        o.Reset();
        return s;
    }
    auto conn = std::make_unique<WinUsbConnection>(o.file, o.usb, in, outPipe, L"USB direto (AOA)", leftover);
    conn->SetMaxPacket(outMaxPacket);
    conn->SetMaxTransfer(maxTransfer);
    out = std::move(conn);
    Log::Info("AOA accessory opened (%s) in=0x%02X out=0x%02X", pick->serial.c_str(), in, outPipe);
    return Status::Ok();
}

}  // namespace celmon
