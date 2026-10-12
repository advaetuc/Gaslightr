#pragma once
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>

namespace Gaslightr {
struct TileRequest {
    uint32_t width = 0;
    uint32_t height = 0;
    int preset = 0;
    bool detailOnly = false;
};
inline void ValidateTile(std::span<const float> rgba, const TileRequest& request) {
    if (request.width == 0 || request.height == 0 || request.width > 2048 || request.height > 2048)
        throw std::invalid_argument("Tiles must be between 1 and 2048 pixels on each axis.");
    if (request.preset < 0 || request.preset > 2)
        throw std::invalid_argument("Performance preset must be 0, 1, or 2.");
    if (request.detailOnly)
        throw std::invalid_argument("Detail-Only is unavailable: CUDA texture interop has not been validated.");
    if (rgba.size() != static_cast<size_t>(request.width) * request.height * 4)
        throw std::invalid_argument("Expected exactly width * height * 4 float32 components.");
    for (size_t i = 0; i < rgba.size(); ++i) {
        if (!std::isfinite(rgba[i]) || std::abs(rgba[i]) > 65504.0f)
            throw std::invalid_argument("Input contains non-finite values or exceeds the float16 finite range.");
        if ((i & 3) == 3 && (rgba[i] < 0.0f || rgba[i] > 1.0f))
            throw std::invalid_argument("Alpha must be in [0, 1].");
    }
}
inline float DecodeHalf(uint16_t half) noexcept {
    const uint32_t sign = static_cast<uint32_t>(half & 0x8000) << 16;
    const uint32_t exponent = (half >> 10) & 31;
    const uint32_t mantissa = half & 1023;
    if (exponent == 0) {
        const float value = std::ldexp(static_cast<float>(mantissa), -24);
        return (sign != 0) ? -value : value;
    }
    const uint32_t bits = sign | ((exponent == 31 ? 255 : exponent + 112) << 23) | (mantissa << 13);
    return std::bit_cast<float>(bits);
}
}
