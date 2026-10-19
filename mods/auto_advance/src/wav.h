// wav.h - the length of a RIFF/WAVE file from its first bytes (auto_advance: how long a voice line is).
//
// The game's voice lines are plain PCM WAVs (data/voice/wav/e8vNNNNN.wav: 48 kHz, mono, 16 bit), so
// the length is the data chunk's size over the fmt chunk's byte rate. Header-only, no Windows, so
// tests/test_auto_advance.cpp checks it offline.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace auto_advance {

inline uint32_t ReadLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// Milliseconds of audio, or 0 when `head` (the file's first `n` bytes) is not a WAVE header with a
// byte rate and a data chunk. The data chunk's size is taken as declared: a header-only read is
// enough, and a truncated file would not play longer than that anyway.
inline double WavDurationMs(const uint8_t* head, size_t n) {
    if (n < 12 || std::memcmp(head, "RIFF", 4) != 0 || std::memcmp(head + 8, "WAVE", 4) != 0) return 0.0;
    uint32_t byte_rate = 0;
    size_t at = 12;
    while (at + 8 <= n) {
        const uint32_t size = ReadLe32(head + at + 4);
        if (std::memcmp(head + at, "fmt ", 4) == 0 && size >= 16 && at + 8 + 12 <= n) {
            byte_rate = ReadLe32(head + at + 8 + 8);
        } else if (std::memcmp(head + at, "data", 4) == 0) {
            if (byte_rate == 0) return 0.0;
            return static_cast<double>(size) * 1000.0 / static_cast<double>(byte_rate);
        }
        at += 8 + static_cast<size_t>(size) + (size & 1u);   // chunks are padded to even sizes
    }
    return 0.0;
}

}  // namespace auto_advance
