#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// Shared control-rate crest geometry for the visible sea and its eight voices.
namespace WaterCrests {
struct Patch {
    float x, z, dx, dz, length, width, height;
};

inline float random01(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15;
    x *= 0x846ca68bu; x ^= x >> 16;
    return float(x & 65535u) / 65535.0f;
}

inline Patch patch(int index, double elapsed) {
    static constexpr float anchors[8][2] = {
        {-8,-4}, {-5,-7}, {1,-8}, {7,-5}, {10,1}, {5,5}, {-1,5}, {-8,2}
    };
    static constexpr float directions[8][2] = {
        {.8f,.6f}, {.4f,.916515f}, {-.2f,.979796f}, {-.8f,.6f},
        {-.98f,-.198997f}, {-.4f,-.916515f}, {.2f,-.979796f}, {.98f,-.198997f}
    };
    const float period = 12.5f + float(index) * .71f;
    const float clock = float(elapsed) / period + float(index) * .618034f;
    const float cycle = std::floor(clock), phase = clock - cycle;
    const uint32_t key = uint32_t(std::max(0.0f, cycle)) * 1664525u
                       + uint32_t(index + 1) * 1013904223u;
    const float r = random01(key), r2 = random01(key + 71u), r3 = random01(key + 173u);
    const float dx = directions[index][0], dz = directions[index][1];
    const float q = phase * (1 - phase);
    return {
        (anchors[index][0] + (r - .5f) * 2.4f + dx * (phase - .5f) * 2.8f) * 1.5f,
        (anchors[index][1] + (r2 - .5f) * 2.4f + dz * (phase - .5f) * 2.8f) * 1.5f,
        dx, dz, 1.5f + r3 * 1.8f, .8f + r * .5f, 1.15f * 16.f * q * q
    };
}
}
