// Encoder output and Annex-B helpers.
#pragma once

#include <cstdint>
#include <vector>

#include "protocol/celmon_protocol.h"

namespace celmon {

struct EncodedFrame {
    std::vector<uint8_t> config;  // parameter sets (SPS/PPS[/VPS]) in Annex-B, only on keyframes
    std::vector<uint8_t> data;    // remaining NAL units, Annex-B
    bool key = false;
    uint64_t captureTimeUs = 0;   // host QPC clock
    uint64_t ptsUs = 0;
    uint32_t encodeUs = 0;        // encoder input -> output
};

// Splits an Annex-B access unit into parameter sets and the rest, and detects IDR/IRAP pictures.
inline void SplitAccessUnit(proto::Codec codec, const uint8_t* p, size_t n, EncodedFrame& out) {
    auto isConfig = [&](uint8_t h) {
        if (codec == proto::Codec::H265) { uint8_t t = (h >> 1) & 0x3F; return t >= 32 && t <= 34; }
        uint8_t t = h & 0x1F; return t == 7 || t == 8;
    };
    auto isKey = [&](uint8_t h) {
        if (codec == proto::Codec::H265) { uint8_t t = (h >> 1) & 0x3F; return t >= 16 && t <= 21; }
        return (h & 0x1F) == 5;
    };
    // Find start codes; each NAL spans [start, next start).
    std::vector<size_t> starts;  // index of the first start-code byte
    std::vector<size_t> heads;   // index of the NAL header byte
    for (size_t i = 0; i + 2 < n;) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
            size_t s = (i > 0 && p[i - 1] == 0) ? i - 1 : i;
            starts.push_back(s);
            heads.push_back(i + 3);
            i += 3;
        } else {
            ++i;
        }
    }
    if (starts.empty()) {  // not Annex-B: pass through untouched
        out.data.assign(p, p + n);
        return;
    }
    for (size_t k = 0; k < starts.size(); ++k) {
        size_t end = k + 1 < starts.size() ? starts[k + 1] : n;
        if (heads[k] >= end) continue;
        uint8_t h = p[heads[k]];
        auto& dst = isConfig(h) ? out.config : out.data;
        dst.insert(dst.end(), p + starts[k], p + end);
        if (isKey(h)) out.key = true;
    }
}

}  // namespace celmon
