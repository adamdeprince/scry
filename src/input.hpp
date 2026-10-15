// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Adam DePrince
#pragma once
#include "renderer.hpp"
#include <functional>

namespace goblin {
enum class Key : uint32_t {
    escape = 0x110000, enter, tab, backspace, up, down, left, right, home, end,
    page_up, page_down, insert, del, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12,
    shift, control, alt, super
};
inline constexpr uint32_t hdr_query_id = UINT32_MAX;
enum class EventType { key, mouse, cell_size, terminal_size, pixel_size, mouse_mode, hdr_format, quit };
struct Event {
    EventType type = EventType::key;
    uint32_t key = 0;
    uint32_t physical = 0; // stable identity across shifted press/release events
    unsigned modifiers = 0; // shift=1, alt=2, control=4, super=8
    int action = 0; // 0=legacy tap, 1=press, 2=repeat, 3=release
    int button = 0, x = 0, y = 0;
    Size size;
};
class Input {
public:
    explicit Input(std::function<void(const Event&)> receive) : receive_(std::move(receive)) {}
    void feed(std::string_view bytes);
    void flush_escape(); // timeout resolves a lone ESC key
    bool pending_escape() const { return pending_ == "\033"; }
private:
    void csi(std::string_view sequence);
    void key(uint32_t key, unsigned modifiers = 0, int action = 0, uint32_t physical = 0);
    std::function<void(const Event&)> receive_;
    std::string pending_;
};
}
