// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Adam DePrince
#include "renderer.hpp"
#include <algorithm>
#include <climits>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace goblin {
std::string base64(std::string_view bytes) {
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    if (bytes.size() > (result.max_size() / 4) * 3 - 2)
        throw std::length_error("base64 allocation overflow");
    result.reserve((bytes.size() + 2) / 3 * 4);
    for (size_t i = 0; i < bytes.size(); i += 3) {
        const auto a = static_cast<uint8_t>(bytes[i]);
        const auto b = i + 1 < bytes.size() ? static_cast<uint8_t>(bytes[i + 1]) : 0;
        const auto c = i + 2 < bytes.size() ? static_cast<uint8_t>(bytes[i + 2]) : 0;
        result += alphabet[a >> 2];
        result += alphabet[((a & 3) << 4) | (b >> 4)];
        result += i + 1 < bytes.size() ? alphabet[((b & 15) << 2) | (c >> 6)] : '=';
        result += i + 2 < bytes.size() ? alphabet[c & 63] : '=';
    }
    return result;
}
void validate_desktop(Size pixels) {
    if (pixels.width <= 0 || pixels.height <= 0)
        throw std::runtime_error("desktop dimensions must be positive");
    if (size_t(pixels.width) > std::numeric_limits<size_t>::max() / 8 / size_t(pixels.height))
        throw std::length_error("desktop framebuffer allocation overflow");
}
Size grid_size(Size pixels, Size cell, int magnification) {
    validate_desktop(pixels);
    if (cell.width <= 0 || cell.height <= 0)
        throw std::runtime_error("cell dimensions must be positive");
    if (size_t(cell.width) > std::string{}.max_size() / 8 / size_t(cell.height))
        throw std::length_error("cell pixel allocation overflow");
    const int64_t width = static_cast<int64_t>(pixels.width) * magnification;
    const int64_t height = static_cast<int64_t>(pixels.height) * magnification;
    if (magnification < 1 || width > INT_MAX || height > INT_MAX)
        throw std::runtime_error("magnification must be positive and enlarged dimensions must fit in an integer");
    return {static_cast<int>((width + cell.width - 1) / cell.width),
            static_cast<int>((height + cell.height - 1) / cell.height)};
}
Renderer::Renderer(Output output, size_t tile_budget) : output_(std::move(output)), tile_budget_(tile_budget) {}
void Renderer::configure(Size desktop, Size cell, int magnification, bool resize_terminal) {
    if (desktop == desktop_ && cell == cell_ && magnification == magnification_) return;
    const auto grid = grid_size(desktop, cell, magnification);
    // Kitty image identifiers are nonzero uint32 values; UINT32_MAX is
    // reserved for our capability query. This is a wire-format constraint.
    const uint64_t required = uint64_t(grid.width) * unsigned(grid.height);
    if (required >= UINT32_MAX || required > std::vector<Tile>{}.max_size())
        throw std::length_error("desktop grid exceeds Kitty image identifiers or addressable tile storage");
    const size_t count = size_t(required);
    if (tile_budget_ && count > tile_budget_) throw std::runtime_error("desktop requires " + std::to_string(count) +
        " image tiles, exceeding --max-tiles; increase terminal image capacity or cell size");
    std::vector<Tile> next(count);
    clear();
    desktop_ = desktop; cell_ = cell; grid_ = grid;
    magnification_ = magnification;
    tiles_ = std::move(next);
    if (resize_terminal)
        output_("\033[8;" + std::to_string(grid.height) + ";" + std::to_string(grid.width) + "t");
}
void Renderer::viewport(Size cells) {
    if (cells != viewport_) { viewport_ = cells; invalidate(); }
}
void Renderer::hdr_output(bool enabled) {
    if (hdr_ != enabled) { hdr_ = enabled; invalidate(); }
}
void Renderer::invalidate() { for (auto& tile : tiles_) { tile.dirty = true; tile.force = true; } }
void Renderer::cursor_shape(Size size, Size hotspot, std::string rgba) {
    if (size.width < 0 || size.height < 0 ||
        (size.height && size_t(size.width) > rgba.max_size() / 4 / size_t(size.height)) ||
        rgba.size() != static_cast<size_t>(size.width) * size.height * 4)
        throw std::runtime_error("invalid cursor shape");
    damage(int64_t(cursor_position_.width) - cursor_hotspot_.width, int64_t(cursor_position_.height) - cursor_hotspot_.height,
           cursor_size_.width, cursor_size_.height);
    cursor_size_ = size; cursor_hotspot_ = hotspot; cursor_rgba_ = std::move(rgba);
    damage(int64_t(cursor_position_.width) - hotspot.width, int64_t(cursor_position_.height) - hotspot.height, size.width, size.height);
}
void Renderer::cursor_position(int x, int y) {
    damage(int64_t(cursor_position_.width) - cursor_hotspot_.width, int64_t(cursor_position_.height) - cursor_hotspot_.height,
           cursor_size_.width, cursor_size_.height);
    cursor_position_ = {x, y};
    damage(int64_t(x) - cursor_hotspot_.width, int64_t(y) - cursor_hotspot_.height, cursor_size_.width, cursor_size_.height);
}
void Renderer::damage(int64_t x, int64_t y, int width, int height) {
    if (tiles_.empty() || width <= 0 || height <= 0) return;
    if (x >= desktop_.width || y >= desktop_.height || x <= -int64_t(width) || y <= -int64_t(height)) return;
    const int x0 = int(std::max<int64_t>(0, x)), y0 = int(std::max<int64_t>(0, y));
    const int x1 = int(std::min<int64_t>(desktop_.width, x + width));
    const int y1 = int(std::min<int64_t>(desktop_.height, y + height));
    if (x0 >= x1 || y0 >= y1) return;
    for (int row = y0 * magnification_ / cell_.height; row <= (y1 * magnification_ - 1) / cell_.height; ++row)
        for (int col = x0 * magnification_ / cell_.width; col <= (x1 * magnification_ - 1) / cell_.width; ++col)
            tiles_[static_cast<size_t>(row) * grid_.width + col].dirty = true;
}
size_t Renderer::paint(const Frame& frame) {
    const bool linear = frame.format == PixelFormat::linear_rgba16;
    const size_t source_bytes = linear ? 8 : 4, pixel_bytes = linear && hdr_ ? 8 : 3;
    if (frame.size != desktop_ || !frame.data || frame.stride < static_cast<size_t>(desktop_.width) * source_bytes ||
        frame.red < 0 || frame.red > 3 || frame.green < 0 || frame.green > 3 || frame.blue < 0 || frame.blue > 3)
        throw std::runtime_error("invalid framebuffer view");
    const size_t row_bytes = size_t(desktop_.width) * source_bytes;
    if (desktop_.height > 1 && frame.stride > (std::numeric_limits<size_t>::max() - row_bytes) / size_t(desktop_.height - 1))
        throw std::length_error("framebuffer stride overflow");
    if (frame.format != last_format_) { last_format_ = frame.format; invalidate(); }
    if (linear && !hdr_ && std::any_of(tiles_.begin(), tiles_.end(), [](const Tile& t) { return t.dirty; })) {
        // One peak for the entire desktop avoids tone-mapping seams between
        // terminal cells. Repaint all cells when that shared curve changes.
        float peak = 1;
        for (int y = 0; y < desktop_.height; ++y) for (int x = 0; x < desktop_.width; ++x)
            for (size_t c = 0; c < 3; ++c)
                peak = std::max(peak, half_read(frame.data + size_t(y) * frame.stride + size_t(x) * 8 + c * 2));
        if (peak != sdr_peak_) { sdr_peak_ = peak; invalidate(); }
    }
    const int cols = std::min(grid_.width, viewport_.width), rows = std::min(grid_.height, viewport_.height);
    size_t changed = 0;
    bool begun = false;
    for (int row = 0; row < rows; ++row) for (int col = 0; col < cols; ++col) {
        const size_t index = static_cast<size_t>(row) * grid_.width + col;
        auto& tile = tiles_[index];
        if (!tile.dirty) continue;
        std::string pixels(static_cast<size_t>(cell_.width) * cell_.height * pixel_bytes, '\0');
        if (pixel_bytes == 8)
            for (size_t i = 0; i < pixels.size(); i += 8) half_write(pixels.data() + i + 6, 1);
        for (int y = 0; y < cell_.height; ++y) {
            const int sy = static_cast<int>((static_cast<int64_t>(row) * cell_.height + y) / magnification_);
            if (sy >= desktop_.height) break;
            auto* dst = pixels.data() + static_cast<size_t>(y) * cell_.width * pixel_bytes;
            for (int x = 0; x < cell_.width; ++x) {
                const int sx = static_cast<int>((static_cast<int64_t>(col) * cell_.width + x) / magnification_);
                if (sx >= desktop_.width) break;
                const auto* src = frame.data + static_cast<size_t>(sy) * frame.stride + size_t(sx) * source_bytes;
                const int64_t cx = int64_t(sx) - cursor_position_.width + cursor_hotspot_.width;
                const int64_t cy = int64_t(sy) - cursor_position_.height + cursor_hotspot_.height;
                const auto* cursor = cx >= 0 && cy >= 0 && cx < cursor_size_.width && cy < cursor_size_.height ?
                    reinterpret_cast<const uint8_t*>(cursor_rgba_.data()) +
                        (static_cast<size_t>(cy) * cursor_size_.width + cx) * 4 : nullptr;
                if (linear) {
                    float rgb[3] = {half_read(src), half_read(src + 2), half_read(src + 4)};
                    if (cursor) for (int c = 0; c < 3; ++c) {
                        const float encoded = cursor[c] / 255.f;
                        const float value = encoded <= .04045f ? encoded / 12.92f : std::pow((encoded + .055f) / 1.055f, 2.4f);
                        const float alpha = cursor[3] / 255.f;
                        rgb[c] = value * alpha + rgb[c] * (1 - alpha);
                    }
                    if (hdr_) {
                        for (int c = 0; c < 3; ++c) half_write(dst + size_t(x) * 8 + c * 2, rgb[c]);
                    } else {
                        const float maximum = std::max({rgb[0], rgb[1], rgb[2]});
                        float scale = 1;
                        if (sdr_peak_ > 1 && maximum > .75f) {
                            const float slope = 4 - 1 / (sdr_peak_ - .75f);
                            scale = (.75f + (maximum - .75f) / (1 + slope * (maximum - .75f))) / maximum;
                        }
                        for (int c = 0; c < 3; ++c) {
                            const float value = std::clamp(rgb[c] * scale, 0.f, 1.f);
                            const float encoded = value <= .0031308f ? 12.92f * value : 1.055f * std::pow(value, 1 / 2.4f) - .055f;
                            dst[size_t(x) * 3 + c] = static_cast<char>(std::lround(encoded * 255));
                        }
                    }
                } else {
                    dst[size_t(x) * 3] = static_cast<char>(src[frame.red]);
                    dst[size_t(x) * 3 + 1] = static_cast<char>(src[frame.green]);
                    dst[size_t(x) * 3 + 2] = static_cast<char>(src[frame.blue]);
                    if (cursor)
                    for (int channel = 0; channel < 3; ++channel)
                        dst[size_t(x) * 3 + channel] = static_cast<char>((cursor[channel] * cursor[3] +
                            static_cast<uint8_t>(dst[size_t(x) * 3 + channel]) * (255 - cursor[3]) + 127) / 255);
                }
            }
        }
        if (tile.shown && !tile.force && tile.pixels == pixels) { tile.dirty = false; continue; }
        if (!begun) { output_("\033[?2026h"); begun = true; }
        std::string command = "\033[" + std::to_string(row + 1) + ";" + std::to_string(col + 1) + "H";
        const auto encoded = base64(pixels);
        for (size_t offset = 0; offset < encoded.size(); offset += 4096) {
            const size_t length = std::min<size_t>(4096, encoded.size() - offset);
            command += "\033_G";
            if (!offset) command += std::string("a=T,t=d,f=") + (pixel_bytes == 8 ? "64" : "24") + ",s=" + std::to_string(cell_.width) + ",v=" +
                std::to_string(cell_.height) + ",i=" + std::to_string(index + 1) + ",p=1,c=1,r=1,C=1,q=2,";
            command += std::string("m=") + (offset + length < encoded.size() ? "1;" : "0;");
            command.append(encoded, offset, length);
            command += "\033\\";
        }
        output_(command);
        tile.pixels = std::move(pixels); tile.shown = true; tile.dirty = false; tile.force = false; ++changed;
    }
    if (begun) output_("\033[H\033[?2026l");
    return changed;
}
void Renderer::clear() {
    std::string commands;
    for (size_t i = 0; i < tiles_.size(); ++i) if (tiles_[i].shown)
        commands += "\033_Ga=d,d=I,i=" + std::to_string(i + 1) + ",q=2;\033\\";
    if (!commands.empty()) output_(commands);
    tiles_.clear(); desktop_ = {}; cell_ = {}; grid_ = {};
}
}
