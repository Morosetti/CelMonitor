// Protocol tests (no framework). Also writes cross-language test vectors checked by the Kotlin ProtocolTest.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "protocol/celmon_protocol.h"

using namespace celmon::proto;

static int g_failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_failures; } \
    } while (0)

static Hello SampleHello() {
    Hello h;
    h.capabilities = CapH264 | CapTouch;
    h.screenWidth = 1080; h.screenHeight = 2400; h.densityDpi = 420; h.refreshRateMilliHz = 60000; h.rotation = 0;
    h.modes = {{2400, 1080, 60, 1}, {1920, 1080, 60, 3}};
    h.deviceId = "abc123"; h.manufacturer = "ACME"; h.model = "Phone X"; h.androidVersion = "14"; h.appVersion = "0.1.0";
    return h;
}

static void TestHelloRoundTrip() {
    auto b = serialize(SampleHello());
    auto h = parseHello(b.data(), b.size());
    CHECK(h.has_value());
    CHECK(h->modes.size() == 2 && h->modes[1].width == 1920 && h->modes[1].codecMask == 3);
    CHECK(h->model == "Phone X" && h->deviceId == "abc123");
    // Truncated payloads are rejected, never read out of bounds.
    for (size_t n = 0; n < b.size(); ++n) CHECK(!parseHello(b.data(), n).has_value() || n == b.size());
}

static void TestHelloValidation() {
    Hello h = SampleHello();
    h.modes = {{100, 100, 60, 1}};
    auto b = serialize(h);
    CHECK(!parseHello(b.data(), b.size()));
    h = SampleHello();
    h.deviceId.clear();
    b = serialize(h);
    CHECK(!parseHello(b.data(), b.size()));
}

static void TestHeader() {
    uint8_t buf[kHeaderSize];
    Header h; h.type = uint16_t(MsgType::Heartbeat); h.length = 24; h.seq = 9;
    encodeHeader(buf, h);
    Header d;
    CHECK(decodeHeader(buf, d) == HeaderStatus::Ok && d.length == 24 && d.seq == 9);
    h.length = kMaxControlPayload + 1;
    encodeHeader(buf, h);
    CHECK(decodeHeader(buf, d) == HeaderStatus::TooLarge);
    h.type = uint16_t(MsgType::VideoFrame); h.length = kMaxControlPayload + 1;
    encodeHeader(buf, h);
    CHECK(decodeHeader(buf, d) == HeaderStatus::Ok);
    h.type = 0x7777; h.length = 3; h.flags = kFlagIgnorable;
    encodeHeader(buf, h);
    CHECK(decodeHeader(buf, d) == HeaderStatus::UnknownIgnorable);
    h.flags = 0;
    encodeHeader(buf, h);
    CHECK(decodeHeader(buf, d) == HeaderStatus::UnknownFatal);
    buf[0] ^= 0xFF;
    CHECK(decodeHeader(buf, d) == HeaderStatus::BadMagic);
}

static void TestInputValidation() {
    InputMouse m; m.action = MouseAction::Down; m.button = MouseButton::Left; m.x = 100; m.y = 200;
    auto b = serialize(m);
    CHECK(parseInputMouse(b.data(), b.size()).has_value());
    b[0] = 9;  // bad action
    CHECK(!parseInputMouse(b.data(), b.size()));
    m.button = MouseButton::None;
    b = serialize(m);
    CHECK(!parseInputMouse(b.data(), b.size()));  // down without a button

    InputTouch t; t.contacts = {{0, TouchAction::Down, 1, 2}, {1, TouchAction::Move, 3, 4}};
    b = serialize(t);
    auto pt = parseInputTouch(b.data(), b.size());
    CHECK(pt && pt->contacts.size() == 2 && pt->contacts[1].x == 3);
    b[4 + 8] = 42;  // contact id out of range
    CHECK(!parseInputTouch(b.data(), b.size()));
}

static void TestClientSettings() {
    ClientSettings s; s.mask = ClientSettings::MaskFps; s.fps = 60;
    auto b = serialize(s);
    CHECK(parseClientSettings(b.data(), b.size()).has_value());
    s.fps = 1000;
    b = serialize(s);
    CHECK(!parseClientSettings(b.data(), b.size()));
    s.mask = 0x80; s.fps = 60;
    b = serialize(s);
    CHECK(!parseClientSettings(b.data(), b.size()));
}

static void WriteVectors() {
    namespace fs = std::filesystem;
    fs::path dir = CELMON_TESTVECTOR_DIR;
    fs::create_directories(dir);
    auto write = [&](const char* name, const std::vector<uint8_t>& b) {
        std::ofstream f(dir / name, std::ios::binary);
        f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
    };
    write("hello.bin", serialize(SampleHello()));
    StreamConfig c; c.streamId = 7; c.width = 1920; c.height = 1080; c.fps = 60; c.codec = Codec::H264; c.bitrateKbps = 12000;
    write("stream_config.bin", serialize(c));
}

int main() {
    TestHelloRoundTrip();
    TestHelloValidation();
    TestHeader();
    TestInputValidation();
    TestClientSettings();
    WriteVectors();
    std::printf(g_failures ? "%d FAILURE(S)\n" : "all protocol tests passed\n", g_failures);
    return g_failures ? 1 : 0;
}
