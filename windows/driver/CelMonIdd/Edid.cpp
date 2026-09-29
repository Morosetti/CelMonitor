// Minimal EDID 1.4 block for a phone monitor: vendor "CLM", per-phone serial, name, preferred timing.
#include "Driver.h"

#include <cstring>

namespace celmon {

namespace {

// Detailed timing descriptor with reduced blanking. Returns false if the mode cannot be expressed (> 4095 px).
bool WriteDetailedTiming(uint8_t* d, const CELMON_MODE& m) {
    const uint32_t hblank = 160, hfront = 48, hsync = 32;
    const uint32_t vblank = 30, vfront = 3, vsync = 5;
    if (m.width > 4095 || m.height > 4095) return false;
    uint64_t clock10k = uint64_t(m.width + hblank) * (m.height + vblank) * m.refreshHz / 10000;
    if (clock10k == 0 || clock10k > 0xFFFF) return false;

    d[0] = uint8_t(clock10k);
    d[1] = uint8_t(clock10k >> 8);
    d[2] = uint8_t(m.width);
    d[3] = uint8_t(hblank);
    d[4] = uint8_t(((m.width >> 8) & 0xF) << 4 | ((hblank >> 8) & 0xF));
    d[5] = uint8_t(m.height);
    d[6] = uint8_t(vblank);
    d[7] = uint8_t(((m.height >> 8) & 0xF) << 4 | ((vblank >> 8) & 0xF));
    d[8] = uint8_t(hfront);
    d[9] = uint8_t(hsync);
    d[10] = uint8_t((vfront & 0xF) << 4 | (vsync & 0xF));
    d[11] = uint8_t(((hfront >> 8) & 3) << 6 | ((hsync >> 8) & 3) << 4 | ((vfront >> 4) & 3) << 2 | ((vsync >> 4) & 3));
    d[12] = d[13] = d[14] = 0;  // image size unknown
    d[15] = d[16] = 0;          // no border
    d[17] = 0x1E;               // digital separate sync, +h +v
    return true;
}

void WriteTextDescriptor(uint8_t* d, uint8_t tag, const char* text) {
    d[0] = d[1] = d[2] = 0;
    d[3] = tag;
    d[4] = 0;
    size_t i = 0;
    for (; i < 13 && text[i]; ++i) d[5 + i] = uint8_t(text[i]);
    if (i < 13) d[5 + i++] = 0x0A;
    for (; i < 13; ++i) d[5 + i] = 0x20;
}

}  // namespace

Edid BuildEdid(uint64_t deviceKey, const char* name, const CELMON_MODE& preferred) {
    Edid e{};
    static const uint8_t header[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    memcpy(e.data(), header, 8);

    // Manufacturer "CLM": three 5-bit letters, big endian.
    uint16_t mfg = uint16_t(('C' - '@') << 10 | ('L' - '@') << 5 | ('M' - '@'));
    e[8] = uint8_t(mfg >> 8);
    e[9] = uint8_t(mfg);
    e[10] = 0x01; e[11] = 0x00;  // product code
    uint32_t serial = uint32_t(deviceKey ^ (deviceKey >> 32));
    e[12] = uint8_t(serial); e[13] = uint8_t(serial >> 8); e[14] = uint8_t(serial >> 16); e[15] = uint8_t(serial >> 24);
    e[16] = 1;                 // week
    e[17] = 2026 - 1990;       // year
    e[18] = 1; e[19] = 4;      // EDID 1.4
    e[20] = 0xA5;              // digital, 8 bpc, DisplayPort-like interface
    e[21] = 0; e[22] = 0;      // physical size unknown -> Windows defaults to 100% scaling
    e[23] = 0x78;              // gamma 2.2
    e[24] = 0x06;              // sRGB default, preferred timing is native
    static const uint8_t chroma[10] = {0xEE, 0x91, 0xA3, 0x54, 0x4C, 0x99, 0x26, 0x0F, 0x50, 0x54};
    memcpy(&e[25], chroma, 10);
    e[35] = e[36] = e[37] = 0;                        // no established timings
    for (int i = 38; i < 54; ++i) e[i] = 0x01;        // no standard timings

    uint8_t* d1 = &e[54];
    if (!WriteDetailedTiming(d1, preferred)) {
        memset(d1, 0, 18);
        d1[3] = 0x10;  // dummy descriptor
    }
    WriteTextDescriptor(&e[72], 0xFC, name);          // monitor name

    char serialText[14] = {};
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < 12; ++i) serialText[i] = hex[(deviceKey >> (60 - 4 * i)) & 0xF];
    WriteTextDescriptor(&e[90], 0xFF, serialText);    // serial string

    // Range limits: 1-240 Hz vertical, 1-255 kHz horizontal, 2550 MHz max clock, no timing formula.
    uint8_t* r = &e[108];
    r[0] = r[1] = r[2] = 0; r[3] = 0xFD; r[4] = 0;
    r[5] = 1; r[6] = 240; r[7] = 1; r[8] = 255; r[9] = 255; r[10] = 0x01;
    for (int i = 11; i < 18; ++i) r[i] = (i == 11) ? 0x0A : 0x20;

    e[126] = 0;  // no extensions
    uint8_t sum = 0;
    for (int i = 0; i < 127; ++i) sum = uint8_t(sum + e[i]);
    e[127] = uint8_t(0x100 - sum);
    return e;
}

}  // namespace celmon
