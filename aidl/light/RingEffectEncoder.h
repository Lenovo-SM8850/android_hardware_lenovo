// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <vector>

namespace lenovo::lightring {

inline std::vector<uint8_t> encodeA(int style, const std::vector<int32_t>& colors,
                                  int direction, int stockSpeed) {
    const int duration = style == 101 ? 64 : stockSpeed == -10 ? 3000 :
                         stockSpeed == 0 ? 1500 : stockSpeed == 10 ? 1000 : 0;
    std::vector<uint8_t> payload = {
        0x00, 0x80, 0x00, 0x01, 0x04, 0x00, 0x00,
        static_cast<uint8_t>(style == 101 ? (direction == 0 ? 2 : 1) : 1),
        0x00, 0x00, static_cast<uint8_t>(duration >> 8), static_cast<uint8_t>(duration),
        0x06, 0x66, static_cast<uint8_t>(style == 102), 0x09, 0x01,
        static_cast<uint8_t>(colors.size()), 0x00, 0x00, 0x00};
    for (uint32_t color : colors) {
        payload.insert(payload.end(), {static_cast<uint8_t>(color >> 16),
                                      static_cast<uint8_t>(color >> 8),
                                      static_cast<uint8_t>(color)});
    }
    return payload;
}

inline std::vector<uint8_t> encodeEffect(int type, std::vector<int32_t> colors,
                                       int speed, int brightness) {
    if (brightness == 0) return {};
    for (int32_t& color : colors) {
        color = ((((color >> 16) & 255) * brightness / 255) << 16) |
                ((((color >> 8) & 255) * brightness / 255) << 8) |
                ((color & 255) * brightness / 255);
    }
    if (type >= 2) return encodeA(type == 2 ? 101 : 102, colors, 0, (speed - 1) * 10);
    const bool breath = type == 1;
    std::vector<uint8_t> payload = {
        0x00, 0x80, 0x01, 0x01, 0x07, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00,
        0x01, 0x09, static_cast<uint8_t>(breath ? 2 : 1), 0, 0, 0, 0, 0, 0,
        0, 1, 2, 3, 4, 5, 6, 7, 8};
    const int duration = breath ? (speed == 0 ? 2000 : speed == 1 ? 1000 : 500) : 1000;
    const uint32_t color = colors.front();
    auto segment = [&](uint8_t start, uint8_t end) {
        payload.insert(payload.end(), {static_cast<uint8_t>(duration >> 8),
            static_cast<uint8_t>(duration), start, end, static_cast<uint8_t>(color >> 16),
            static_cast<uint8_t>(color >> 8), static_cast<uint8_t>(color)});
    };
    segment(breath ? 0 : 0xcc, 0xcc);
    if (breath) segment(0xcc, 0);
    return payload;
}
}  // namespace lenovo::lightring
