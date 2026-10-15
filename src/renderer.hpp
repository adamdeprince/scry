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
enum class PixelFormat { rgbx8, linear_rgba16 };
// rgbx8 offsets describe bytes in memory. linear_rgba16 is little-endian
// IEEE binary16 RGBA in extended linear sRGB (1.0 = SDR white).
struct Frame {
    const uint8_t* data; Size size; size_t stride; int red, green, blue;
    PixelFormat format = PixelFormat::rgbx8;
};
float half_read(const uint8_t* bytes);
void half_write(char* bytes, float value);
using Output = std::function<void(std::string_view)>;
std::string base64(std::string_view bytes);
Size grid_size(Size pixels, Size cell, int magnification = 1);
void validate_desktop(Size pixels);

class Renderer {
public:
    explicit Renderer(Output output, size_t tile_budget = 0);
    void configure(Size desktop, Size cell, int magnification = 1, bool resize_terminal = true);
    void viewport(Size cells);
    void hdr_output(bool enabled);
    void damage(int64_t x, int64_t y, int width, int height);
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
    bool hdr_ = false;
    PixelFormat last_format_ = PixelFormat::rgbx8;
    float sdr_peak_ = 1;
    std::vector<Tile> tiles_;
    Size cursor_size_, cursor_hotspot_, cursor_position_;
    std::string cursor_rgba_;
};
}
