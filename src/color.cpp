// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Adam DePrince
#include "renderer.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace goblin {
float half_read(const uint8_t* bytes) {
    const unsigned bits = bytes[0] | unsigned(bytes[1]) << 8;
    const unsigned exponent = (bits >> 10) & 31, fraction = bits & 1023;
    if (exponent == 31) throw std::runtime_error("non-finite HDR pixel");
    const float value = exponent ? std::ldexp(float(1024 + fraction), int(exponent) - 25)
                                 : std::ldexp(float(fraction), -24);
    return bits & 32768 ? -value : value;
}
void half_write(char* bytes, float value) {
    if (!std::isfinite(value) || std::abs(value) > 65504)
        throw std::runtime_error("HDR pixel cannot be represented as binary16");
    const unsigned sign = std::signbit(value) ? 32768 : 0;
    double magnitude = std::abs(double(value));
    int exponent = 0;
    std::frexp(magnitude, &exponent);
    const int encoded_exponent = magnitude == 0 ? 0 : std::max(0, exponent + 14);
    const double scaled = encoded_exponent ? std::ldexp(magnitude, 25 - encoded_exponent) - 1024
                                            : std::ldexp(magnitude, 24);
    const auto integral = static_cast<unsigned>(std::floor(scaled));
    const double remainder = scaled - integral;
    const unsigned rounded = integral + (remainder > .5 || (remainder == .5 && (integral & 1)));
    const unsigned bits = sign | ((unsigned(encoded_exponent) << 10) + rounded);
    bytes[0] = char(bits & 255); bytes[1] = char(bits >> 8);
}
}
