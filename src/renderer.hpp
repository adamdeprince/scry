// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Adam DePrince
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace goblin {
struct Size { int width = 0, height = 0; };
inline bool operator==(Size a, Size b) { return a.width == b.width && a.height == b.height; }
inline bool operator!=(Size a, Size b) { return !(a == b); }
// A null data pointer means the backend has not received a frame yet.
// Four-byte source pixels. Offsets describe bytes in memory, independent of host endianness.
struct Frame { const uint8_t* data; Size size; size_t stride; int red, green, blue; };
using Output = std::function<void(std::string_view)>;
std::string base64(std::string_view bytes);
Size grid_size(Size pixels, Size cell, int magnification = 1);
void validate_desktop(Size pixels);

class Renderer {
public:
    explicit Renderer(Output output, size_t tile_budget = 262144);
    void configure(Size desktop, Size cell, int magnification = 1, bool resize_terminal = true);
    void viewport(Size cells);
    void damage(int x, int y, int width, int height);
    void invalidate();
    void cursor_shape(Size size, Size hotspot, std::string rgba);
    void cursor_position(int x, int y);
    size_t paint(const Frame& frame);
    void clear(); // deletes only images owned by this renderer
    Size desktop() const { return desktop_; }
    Size cell() const { return cell_; }
    Size grid() const { return grid_; }
private:
    struct Tile { bool dirty = true, shown = false, force = true; std::string pixels; };
    Output output_;
    size_t tile_budget_;
    Size desktop_, cell_, grid_, viewport_;
    int magnification_ = 1;
    std::vector<Tile> tiles_;
    Size cursor_size_, cursor_hotspot_, cursor_position_;
    std::string cursor_rgba_;
};
}
