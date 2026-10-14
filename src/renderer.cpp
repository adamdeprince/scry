// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Adam DePrince
#include "renderer.hpp"
#include <algorithm>
#include <climits>
#include <stdexcept>

namespace goblin {
std::string base64(std::string_view bytes) {
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
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
    if (pixels.width <= 0 || pixels.height <= 0 || pixels.width > 32768 || pixels.height > 32768 ||
        static_cast<int64_t>(pixels.width) * pixels.height > 32 * 1024 * 1024)
        throw std::runtime_error("desktop exceeds limit (32768 per dimension, 32 million pixels)");
}
Size grid_size(Size pixels, Size cell, int magnification) {
    validate_desktop(pixels);
    if (cell.width <= 0 || cell.height <= 0 || cell.width > 512 || cell.height > 512)
        throw std::runtime_error("cell size must be between 1 and 512 pixels per dimension");
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
    const size_t count = static_cast<size_t>(grid.width) * grid.height;
    if (count > tile_budget_) throw std::runtime_error("desktop requires " + std::to_string(count) +
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
void Renderer::invalidate() { for (auto& tile : tiles_) { tile.dirty = true; tile.force = true; } }
void Renderer::cursor_shape(Size size, Size hotspot, std::string rgba) {
    if (size.width < 0 || size.height < 0 || size.width > 256 || size.height > 256 ||
        rgba.size() != static_cast<size_t>(size.width) * size.height * 4)
        throw std::runtime_error("invalid cursor shape");
    damage(cursor_position_.width - cursor_hotspot_.width, cursor_position_.height - cursor_hotspot_.height,
           cursor_size_.width, cursor_size_.height);
    cursor_size_ = size; cursor_hotspot_ = hotspot; cursor_rgba_ = std::move(rgba);
    damage(cursor_position_.width - hotspot.width, cursor_position_.height - hotspot.height, size.width, size.height);
}
void Renderer::cursor_position(int x, int y) {
    damage(cursor_position_.width - cursor_hotspot_.width, cursor_position_.height - cursor_hotspot_.height,
           cursor_size_.width, cursor_size_.height);
    cursor_position_ = {x, y};
    damage(x - cursor_hotspot_.width, y - cursor_hotspot_.height, cursor_size_.width, cursor_size_.height);
}
void Renderer::damage(int x, int y, int width, int height) {
    if (tiles_.empty() || width <= 0 || height <= 0) return;
    const int x0 = std::max(0, x), y0 = std::max(0, y);
    const int x1 = static_cast<int>(std::min<int64_t>(desktop_.width, static_cast<int64_t>(x) + width));
    const int y1 = static_cast<int>(std::min<int64_t>(desktop_.height, static_cast<int64_t>(y) + height));
    if (x0 >= x1 || y0 >= y1) return;
    for (int row = y0 * magnification_ / cell_.height; row <= (y1 * magnification_ - 1) / cell_.height; ++row)
        for (int col = x0 * magnification_ / cell_.width; col <= (x1 * magnification_ - 1) / cell_.width; ++col)
            tiles_[static_cast<size_t>(row) * grid_.width + col].dirty = true;
}
size_t Renderer::paint(const Frame& frame) {
    if (frame.size != desktop_ || !frame.data || frame.stride < static_cast<size_t>(desktop_.width) * 4 ||
        frame.red < 0 || frame.red > 3 || frame.green < 0 || frame.green > 3 || frame.blue < 0 || frame.blue > 3)
        throw std::runtime_error("invalid framebuffer view");
    const int cols = std::min(grid_.width, viewport_.width), rows = std::min(grid_.height, viewport_.height);
    size_t changed = 0;
    bool begun = false;
    for (int row = 0; row < rows; ++row) for (int col = 0; col < cols; ++col) {
        const size_t index = static_cast<size_t>(row) * grid_.width + col;
        auto& tile = tiles_[index];
        if (!tile.dirty) continue;
        std::string pixels(static_cast<size_t>(cell_.width) * cell_.height * 3, '\0');
        for (int y = 0; y < cell_.height; ++y) {
            const int sy = static_cast<int>((static_cast<int64_t>(row) * cell_.height + y) / magnification_);
            if (sy >= desktop_.height) break;
            auto* dst = pixels.data() + static_cast<size_t>(y) * cell_.width * 3;
            for (int x = 0; x < cell_.width; ++x) {
                const int sx = static_cast<int>((static_cast<int64_t>(col) * cell_.width + x) / magnification_);
                if (sx >= desktop_.width) break;
                const auto* src = frame.data + static_cast<size_t>(sy) * frame.stride + sx * 4;
                dst[x * 3] = static_cast<char>(src[frame.red]);
                dst[x * 3 + 1] = static_cast<char>(src[frame.green]);
                dst[x * 3 + 2] = static_cast<char>(src[frame.blue]);
                const int cx = sx - cursor_position_.width + cursor_hotspot_.width;
                const int cy = sy - cursor_position_.height + cursor_hotspot_.height;
                if (cx >= 0 && cy >= 0 && cx < cursor_size_.width && cy < cursor_size_.height) {
                    const auto* cursor = reinterpret_cast<const uint8_t*>(cursor_rgba_.data()) +
                        (static_cast<size_t>(cy) * cursor_size_.width + cx) * 4;
                    for (int channel = 0; channel < 3; ++channel)
                        dst[x * 3 + channel] = static_cast<char>((cursor[channel] * cursor[3] +
                            static_cast<uint8_t>(dst[x * 3 + channel]) * (255 - cursor[3]) + 127) / 255);
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
            if (!offset) command += "a=T,t=d,f=24,s=" + std::to_string(cell_.width) + ",v=" +
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
