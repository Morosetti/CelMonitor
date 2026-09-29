// CelMonitor wire protocol v1 — reference C++ implementation (header-only).
// Spec: docs/PROTOCOLO.md. Kotlin mirror: android/.../protocol/. Keep values identical.
#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace celmon::proto {

constexpr uint32_t kMagic = 0x4D4C4543;  // "CELM"
constexpr uint16_t kVersionMajor = 1;
constexpr uint16_t kVersionMinor = 0;
constexpr size_t kHeaderSize = 16;
constexpr uint32_t kMaxVideoPayload = 8u * 1024 * 1024;
constexpr uint32_t kMaxControlPayload = 4u * 1024;
constexpr size_t kMaxString = 256;
constexpr uint16_t kFlagIgnorable = 0x0001;

enum class MsgType : uint16_t {
    Hello = 0x0001,
    HelloAck = 0x0002,
    StreamConfig = 0x0003,
    StreamReady = 0x0004,
    VideoFrame = 0x0010,
    FrameAck = 0x0011,
    RequestKeyframe = 0x0012,
    Heartbeat = 0x0020,
    ClientSettings = 0x0021,
    HostStatus = 0x0022,
    InputMouse = 0x0030,
    InputTouch = 0x0031,
    Notice = 0x00F0,
    Disconnect = 0x00FF,
};

enum Capability : uint32_t {
    CapH264 = 1u << 0,
    CapH265 = 1u << 1,
    CapAV1 = 1u << 2,
    CapTouch = 1u << 8,
    CapLowLatencyDecoder = 1u << 9,
};

enum class Codec : uint8_t { H264 = 1, H265 = 2, AV1 = 3 };

enum class DisconnectReason : uint16_t {
    Normal = 0,
    ProtocolError = 1,
    VersionMismatch = 2,
    Timeout = 3,
    DisplayError = 4,
    EncoderError = 5,
    DecoderError = 6,
    Shutdown = 7,
};

enum FrameFlags : uint16_t { FrameKey = 1u << 0, FrameCodecConfig = 1u << 1 };

enum class MouseAction : uint8_t { Move = 1, Down = 2, Up = 3, Wheel = 4 };
enum class MouseButton : uint8_t { None = 0, Left = 1, Right = 2, Middle = 3 };
enum class TouchAction : uint8_t { Down = 1, Move = 2, Up = 3, Cancel = 4 };

constexpr uint32_t kMinDimension = 320;
constexpr uint32_t kMaxDimension = 7680;
constexpr uint16_t kMaxFps = 240;
constexpr size_t kMaxModes = 32;
constexpr size_t kMaxTouchContacts = 10;

struct Header {
    uint32_t magic = kMagic;
    uint16_t type = 0;
    uint16_t flags = 0;
    uint32_t length = 0;
    uint32_t seq = 0;
};

// ---------------------------------------------------------------- messages

struct DisplayMode {
    uint32_t width = 0;
    uint32_t height = 0;
    uint16_t maxFps = 0;
    uint16_t codecMask = 0;  // bit (codec-1)
};

struct Hello {
    uint16_t versionMajor = kVersionMajor;
    uint16_t versionMinor = kVersionMinor;
    uint32_t capabilities = 0;
    uint32_t screenWidth = 0;
    uint32_t screenHeight = 0;
    uint32_t densityDpi = 0;
    uint32_t refreshRateMilliHz = 0;
    uint32_t rotation = 0;
    std::vector<DisplayMode> modes;
    std::string deviceId, manufacturer, model, androidVersion, appVersion;
};

struct HelloAck {
    uint16_t versionMajor = kVersionMajor;
    uint16_t versionMinor = kVersionMinor;
    uint32_t hostCapabilities = 0;
    uint32_t sessionId = 0;
    std::string hostName;
};

struct StreamConfig {
    uint32_t streamId = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint16_t fps = 0;
    Codec codec = Codec::H264;
    uint8_t orientation = 0;  // 0 landscape, 1 portrait
    uint32_t bitrateKbps = 0;
    uint32_t flags = 0;
};

struct StreamReady {
    uint32_t streamId = 0;
    uint32_t status = 0;
};

// VIDEO_FRAME fixed part; bitstream follows in the payload.
struct VideoFrameHeader {
    uint32_t streamId = 0;
    uint32_t frameNumber = 0;
    uint64_t captureTimeUs = 0;
    uint64_t ptsUs = 0;
    uint16_t flags = 0;
};
constexpr size_t kVideoFrameHeaderSize = 28;

struct FrameAck {
    enum : uint32_t { Rendered = 1 };
    uint32_t streamId = 0;
    uint32_t frameNumber = 0;
    uint64_t captureTimeUs = 0;
    uint32_t clientProcessingUs = 0;
    uint32_t flags = 0;
};

struct RequestKeyframe {
    uint32_t streamId = 0;
    uint32_t reason = 0;
};

struct Heartbeat {
    uint64_t senderTimeUs = 0;
    uint64_t echoTimeUs = 0;
    uint32_t counter = 0;
};

struct ClientSettings {
    enum : uint32_t { MaskFps = 1, MaskQuality = 2, MaskOrientation = 4 };
    uint32_t mask = 0;
    uint16_t fps = 0;
    uint8_t quality = 0;
    uint8_t orientation = 0;
};

struct HostStatus {
    uint32_t encodeFpsX100 = 0;
    uint32_t bitrateKbps = 0;
    uint32_t latencyUs = 0;
    uint32_t flags = 0;
    std::string encoderName;
};

struct InputMouse {
    MouseAction action = MouseAction::Move;
    MouseButton button = MouseButton::None;
    uint16_t x = 0, y = 0;
    int16_t wheelV = 0, wheelH = 0;
};

struct TouchContact {
    uint8_t id = 0;
    TouchAction action = TouchAction::Move;
    uint16_t x = 0, y = 0;
};

struct InputTouch {
    std::vector<TouchContact> contacts;
};

struct Notice {
    uint16_t code = 0;
    uint16_t severity = 0;
    std::string message;
};

struct Disconnect {
    DisconnectReason reason = DisconnectReason::Normal;
    std::string message;
};

// ---------------------------------------------------------------- byte I/O

class Writer {
public:
    void u8(uint8_t v) { buf_.push_back(v); }
    void u16(uint16_t v) { for (int i = 0; i < 2; ++i) buf_.push_back(uint8_t(v >> (8 * i))); }
    void u32(uint32_t v) { for (int i = 0; i < 4; ++i) buf_.push_back(uint8_t(v >> (8 * i))); }
    void u64(uint64_t v) { for (int i = 0; i < 8; ++i) buf_.push_back(uint8_t(v >> (8 * i))); }
    void i16(int16_t v) { u16(uint16_t(v)); }
    void str(const std::string& s) {
        size_t n = s.size() > kMaxString ? kMaxString : s.size();
        u16(uint16_t(n));
        buf_.insert(buf_.end(), s.begin(), s.begin() + n);
    }
    void bytes(const void* p, size_t n) {
        auto b = static_cast<const uint8_t*>(p);
        buf_.insert(buf_.end(), b, b + n);
    }
    std::vector<uint8_t>& data() { return buf_; }

private:
    std::vector<uint8_t> buf_;
};

// Bounds-checked reader. Any out-of-range read sets ok() == false and returns 0.
class Reader {
public:
    Reader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    bool ok() const { return ok_; }
    size_t remaining() const { return n_ - off_; }
    const uint8_t* cursor() const { return p_ + off_; }

    uint8_t u8() { return uint8_t(take(1)); }
    uint16_t u16() { return uint16_t(take(2)); }
    uint32_t u32() { return uint32_t(take(4)); }
    uint64_t u64() { return take(8); }
    int16_t i16() { return int16_t(u16()); }
    std::string str() {
        uint16_t n = u16();
        if (!ok_ || n > kMaxString || n > remaining()) { ok_ = false; return {}; }
        std::string s(reinterpret_cast<const char*>(p_ + off_), n);
        off_ += n;
        return s;
    }

private:
    uint64_t take(size_t k) {
        if (!ok_ || remaining() < k) { ok_ = false; return 0; }
        uint64_t v = 0;
        for (size_t i = 0; i < k; ++i) v |= uint64_t(p_[off_ + i]) << (8 * i);
        off_ += k;
        return v;
    }
    const uint8_t* p_;
    size_t n_;
    size_t off_ = 0;
    bool ok_ = true;
};

// ---------------------------------------------------------------- header

inline void encodeHeader(uint8_t out[kHeaderSize], const Header& h) {
    Writer w;
    w.u32(h.magic); w.u16(h.type); w.u16(h.flags); w.u32(h.length); w.u32(h.seq);
    std::memcpy(out, w.data().data(), kHeaderSize);
}

inline bool isKnownType(uint16_t t) {
    switch (MsgType(t)) {
    case MsgType::Hello: case MsgType::HelloAck: case MsgType::StreamConfig:
    case MsgType::StreamReady: case MsgType::VideoFrame: case MsgType::FrameAck:
    case MsgType::RequestKeyframe: case MsgType::Heartbeat: case MsgType::ClientSettings:
    case MsgType::HostStatus: case MsgType::InputMouse: case MsgType::InputTouch:
    case MsgType::Notice: case MsgType::Disconnect:
        return true;
    }
    return false;
}

inline uint32_t maxPayloadFor(uint16_t t) {
    return MsgType(t) == MsgType::VideoFrame ? kMaxVideoPayload : kMaxControlPayload;
}

enum class HeaderStatus { Ok, BadMagic, TooLarge, UnknownIgnorable, UnknownFatal };

inline HeaderStatus decodeHeader(const uint8_t in[kHeaderSize], Header& h) {
    Reader r(in, kHeaderSize);
    h.magic = r.u32(); h.type = r.u16(); h.flags = r.u16(); h.length = r.u32(); h.seq = r.u32();
    if (h.magic != kMagic) return HeaderStatus::BadMagic;
    if (!isKnownType(h.type)) {
        // Unknown messages still must fit the control limit so we can skip them safely.
        if (h.length > kMaxControlPayload) return HeaderStatus::TooLarge;
        return (h.flags & kFlagIgnorable) ? HeaderStatus::UnknownIgnorable : HeaderStatus::UnknownFatal;
    }
    if (h.length > maxPayloadFor(h.type)) return HeaderStatus::TooLarge;
    return HeaderStatus::Ok;
}

// ---------------------------------------------------------------- validation helpers

inline bool validDimension(uint32_t v) { return v >= kMinDimension && v <= kMaxDimension; }
inline bool validFps(uint32_t v) { return v >= 1 && v <= kMaxFps; }

// ---------------------------------------------------------------- serialize

inline std::vector<uint8_t> serialize(const Hello& m) {
    Writer w;
    w.u16(m.versionMajor); w.u16(m.versionMinor); w.u32(m.capabilities);
    w.u32(m.screenWidth); w.u32(m.screenHeight); w.u32(m.densityDpi);
    w.u32(m.refreshRateMilliHz); w.u32(m.rotation);
    size_t n = m.modes.size() > kMaxModes ? kMaxModes : m.modes.size();
    w.u16(uint16_t(n)); w.u16(0); w.u32(0);
    for (size_t i = 0; i < n; ++i) {
        w.u32(m.modes[i].width); w.u32(m.modes[i].height);
        w.u16(m.modes[i].maxFps); w.u16(m.modes[i].codecMask);
    }
    w.str(m.deviceId); w.str(m.manufacturer); w.str(m.model); w.str(m.androidVersion); w.str(m.appVersion);
    return std::move(w.data());
}

inline std::vector<uint8_t> serialize(const HelloAck& m) {
    Writer w;
    w.u16(m.versionMajor); w.u16(m.versionMinor); w.u32(m.hostCapabilities); w.u32(m.sessionId);
    w.str(m.hostName);
    return std::move(w.data());
}

inline std::vector<uint8_t> serialize(const StreamConfig& m) {
    Writer w;
    w.u32(m.streamId); w.u32(m.width); w.u32(m.height); w.u16(m.fps);
    w.u8(uint8_t(m.codec)); w.u8(m.orientation); w.u32(m.bitrateKbps); w.u32(m.flags);
    return std::move(w.data());
}

inline std::vector<uint8_t> serialize(const StreamReady& m) {
    Writer w; w.u32(m.streamId); w.u32(m.status); return std::move(w.data());
}

inline void serializeVideoFrameHeader(Writer& w, const VideoFrameHeader& m) {
    w.u32(m.streamId); w.u32(m.frameNumber); w.u64(m.captureTimeUs); w.u64(m.ptsUs);
    w.u16(m.flags); w.u16(0);
}

inline std::vector<uint8_t> serialize(const FrameAck& m) {
    Writer w;
    w.u32(m.streamId); w.u32(m.frameNumber); w.u64(m.captureTimeUs); w.u32(m.clientProcessingUs); w.u32(m.flags);
    return std::move(w.data());
}

inline std::vector<uint8_t> serialize(const RequestKeyframe& m) {
    Writer w; w.u32(m.streamId); w.u32(m.reason); return std::move(w.data());
}

inline std::vector<uint8_t> serialize(const Heartbeat& m) {
    Writer w; w.u64(m.senderTimeUs); w.u64(m.echoTimeUs); w.u32(m.counter); w.u32(0);
    return std::move(w.data());
}

inline std::vector<uint8_t> serialize(const ClientSettings& m) {
    Writer w; w.u32(m.mask); w.u16(m.fps); w.u8(m.quality); w.u8(m.orientation); w.u32(0);
    return std::move(w.data());
}

inline std::vector<uint8_t> serialize(const HostStatus& m) {
    Writer w;
    w.u32(m.encodeFpsX100); w.u32(m.bitrateKbps); w.u32(m.latencyUs); w.u32(m.flags); w.u32(0);
    w.str(m.encoderName);
    return std::move(w.data());
}

inline std::vector<uint8_t> serialize(const InputMouse& m) {
    Writer w;
    w.u8(uint8_t(m.action)); w.u8(uint8_t(m.button)); w.u16(0);
    w.u16(m.x); w.u16(m.y); w.i16(m.wheelV); w.i16(m.wheelH);
    return std::move(w.data());
}

inline std::vector<uint8_t> serialize(const InputTouch& m) {
    Writer w;
    size_t n = m.contacts.size() > kMaxTouchContacts ? kMaxTouchContacts : m.contacts.size();
    w.u8(uint8_t(n)); w.u8(0); w.u8(0); w.u8(0);
    for (size_t i = 0; i < n; ++i) {
        const auto& c = m.contacts[i];
        w.u8(c.id); w.u8(uint8_t(c.action)); w.u16(0); w.u16(c.x); w.u16(c.y);
    }
    return std::move(w.data());
}

inline std::vector<uint8_t> serialize(const Notice& m) {
    Writer w; w.u16(m.code); w.u16(m.severity); w.str(m.message); return std::move(w.data());
}

inline std::vector<uint8_t> serialize(const Disconnect& m) {
    Writer w; w.u16(uint16_t(m.reason)); w.u16(0); w.str(m.message); return std::move(w.data());
}

// ---------------------------------------------------------------- parse
// Each parser validates structure and value ranges. Trailing bytes are ignored (forward compat).

inline std::optional<Hello> parseHello(const uint8_t* p, size_t n) {
    Reader r(p, n);
    Hello m;
    m.versionMajor = r.u16(); m.versionMinor = r.u16(); m.capabilities = r.u32();
    m.screenWidth = r.u32(); m.screenHeight = r.u32(); m.densityDpi = r.u32();
    m.refreshRateMilliHz = r.u32(); m.rotation = r.u32();
    uint16_t count = r.u16(); r.u16(); r.u32();
    if (!r.ok()) return std::nullopt;
    if (m.versionMajor != kVersionMajor) return m;  // caller rejects with VERSION_MISMATCH
    if (count == 0 || count > kMaxModes || m.rotation > 3) return std::nullopt;
    if (!validDimension(m.screenWidth) || !validDimension(m.screenHeight)) return std::nullopt;
    for (uint16_t i = 0; i < count; ++i) {
        DisplayMode d;
        d.width = r.u32(); d.height = r.u32(); d.maxFps = r.u16(); d.codecMask = r.u16();
        if (!r.ok() || !validDimension(d.width) || !validDimension(d.height) || !validFps(d.maxFps) ||
            d.codecMask == 0)
            return std::nullopt;
        m.modes.push_back(d);
    }
    m.deviceId = r.str(); m.manufacturer = r.str(); m.model = r.str();
    m.androidVersion = r.str(); m.appVersion = r.str();
    if (!r.ok() || m.deviceId.empty()) return std::nullopt;
    return m;
}

inline std::optional<HelloAck> parseHelloAck(const uint8_t* p, size_t n) {
    Reader r(p, n);
    HelloAck m;
    m.versionMajor = r.u16(); m.versionMinor = r.u16(); m.hostCapabilities = r.u32(); m.sessionId = r.u32();
    m.hostName = r.str();
    if (!r.ok()) return std::nullopt;
    return m;
}

inline std::optional<StreamConfig> parseStreamConfig(const uint8_t* p, size_t n) {
    Reader r(p, n);
    StreamConfig m;
    m.streamId = r.u32(); m.width = r.u32(); m.height = r.u32(); m.fps = r.u16();
    uint8_t codec = r.u8(); m.orientation = r.u8(); m.bitrateKbps = r.u32(); m.flags = r.u32();
    if (!r.ok() || !validDimension(m.width) || !validDimension(m.height) || !validFps(m.fps) ||
        codec < 1 || codec > 3 || m.orientation > 1)
        return std::nullopt;
    m.codec = Codec(codec);
    return m;
}

inline std::optional<StreamReady> parseStreamReady(const uint8_t* p, size_t n) {
    Reader r(p, n);
    StreamReady m; m.streamId = r.u32(); m.status = r.u32();
    if (!r.ok()) return std::nullopt;
    return m;
}

// Returns header; bitstream = [p + kVideoFrameHeaderSize, p + n).
inline std::optional<VideoFrameHeader> parseVideoFrameHeader(const uint8_t* p, size_t n) {
    Reader r(p, n);
    VideoFrameHeader m;
    m.streamId = r.u32(); m.frameNumber = r.u32(); m.captureTimeUs = r.u64(); m.ptsUs = r.u64();
    m.flags = r.u16(); r.u16();
    if (!r.ok() || r.remaining() == 0) return std::nullopt;
    return m;
}

inline std::optional<FrameAck> parseFrameAck(const uint8_t* p, size_t n) {
    Reader r(p, n);
    FrameAck m;
    m.streamId = r.u32(); m.frameNumber = r.u32(); m.captureTimeUs = r.u64(); m.clientProcessingUs = r.u32();
    m.flags = r.u32();
    if (!r.ok()) return std::nullopt;
    return m;
}

inline std::optional<RequestKeyframe> parseRequestKeyframe(const uint8_t* p, size_t n) {
    Reader r(p, n);
    RequestKeyframe m; m.streamId = r.u32(); m.reason = r.u32();
    if (!r.ok()) return std::nullopt;
    return m;
}

inline std::optional<Heartbeat> parseHeartbeat(const uint8_t* p, size_t n) {
    Reader r(p, n);
    Heartbeat m; m.senderTimeUs = r.u64(); m.echoTimeUs = r.u64(); m.counter = r.u32(); r.u32();
    if (!r.ok()) return std::nullopt;
    return m;
}

inline std::optional<ClientSettings> parseClientSettings(const uint8_t* p, size_t n) {
    Reader r(p, n);
    ClientSettings m; m.mask = r.u32(); m.fps = r.u16(); m.quality = r.u8(); m.orientation = r.u8(); r.u32();
    if (!r.ok() || (m.mask & ~7u)) return std::nullopt;
    if ((m.mask & ClientSettings::MaskFps) && !validFps(m.fps)) return std::nullopt;
    if ((m.mask & ClientSettings::MaskQuality) && (m.quality < 1 || m.quality > 100)) return std::nullopt;
    if ((m.mask & ClientSettings::MaskOrientation) && m.orientation > 1) return std::nullopt;
    return m;
}

inline std::optional<HostStatus> parseHostStatus(const uint8_t* p, size_t n) {
    Reader r(p, n);
    HostStatus m;
    m.encodeFpsX100 = r.u32(); m.bitrateKbps = r.u32(); m.latencyUs = r.u32(); m.flags = r.u32(); r.u32();
    m.encoderName = r.str();
    if (!r.ok()) return std::nullopt;
    return m;
}

inline std::optional<InputMouse> parseInputMouse(const uint8_t* p, size_t n) {
    Reader r(p, n);
    InputMouse m;
    uint8_t action = r.u8(), button = r.u8(); r.u16();
    m.x = r.u16(); m.y = r.u16(); m.wheelV = r.i16(); m.wheelH = r.i16();
    if (!r.ok() || action < 1 || action > 4 || button > 3) return std::nullopt;
    m.action = MouseAction(action);
    m.button = MouseButton(button);
    if ((m.action == MouseAction::Down || m.action == MouseAction::Up) && m.button == MouseButton::None)
        return std::nullopt;
    return m;
}

inline std::optional<InputTouch> parseInputTouch(const uint8_t* p, size_t n) {
    Reader r(p, n);
    uint8_t count = r.u8(); r.u8(); r.u8(); r.u8();
    if (!r.ok() || count == 0 || count > kMaxTouchContacts) return std::nullopt;
    InputTouch m;
    for (uint8_t i = 0; i < count; ++i) {
        TouchContact c;
        c.id = r.u8(); uint8_t action = r.u8(); r.u16(); c.x = r.u16(); c.y = r.u16();
        if (!r.ok() || c.id >= kMaxTouchContacts || action < 1 || action > 4) return std::nullopt;
        c.action = TouchAction(action);
        m.contacts.push_back(c);
    }
    return m;
}

inline std::optional<Notice> parseNotice(const uint8_t* p, size_t n) {
    Reader r(p, n);
    Notice m; m.code = r.u16(); m.severity = r.u16(); m.message = r.str();
    if (!r.ok() || m.severity > 2) return std::nullopt;
    return m;
}

inline std::optional<Disconnect> parseDisconnect(const uint8_t* p, size_t n) {
    Reader r(p, n);
    Disconnect m; m.reason = DisconnectReason(r.u16()); r.u16(); m.message = r.str();
    if (!r.ok()) return std::nullopt;
    return m;
}

}  // namespace celmon::proto
