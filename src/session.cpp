// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Adam DePrince
#include "session.hpp"
#include <algorithm>
#include <climits>
#include <stdexcept>

namespace goblin {
Session::Session(Terminal& terminal, const Options& options)
    : terminal_(terminal), renderer_([&terminal](std::string_view bytes) { terminal.write(bytes); }, options.max_tiles),
      input_([this](const Event& value) { event(value); }), cell_(options.cell),
      fixed_cell_(options.cell.width > 0), view_only_(options.view_only), magnification_(options.magnification) {
    local_ = options.protocol == "wayland" || options.protocol == "x11";
    if (magnification_ < 1) throw std::runtime_error("magnification must be a positive integer");
    viewport_ = terminal.cells(); renderer_.viewport(viewport_);
    pixels_ = terminal.pixels();
    // Allow authoritative XTWINOPS replies to supersede ioctl-derived geometry.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (!stopping() && std::chrono::steady_clock::now() < deadline) poll(20);
    if (cell_.width <= 0 || cell_.height <= 0) cell_ = terminal.cell_pixels();
    if (cell_.width <= 0 || cell_.height <= 0)
        throw std::runtime_error("terminal did not report cell pixels; supply --cell-size WIDTHxHEIGHT");
    grid_size({1, 1}, cell_);
    if (local_) pending_display_ = display_size();
}
Session::~Session() { try { renderer_.clear(); terminal_.flush(); } catch (...) {} }
void Session::resize(Size desktop) {
    renderer_.configure(desktop, cell_, magnification_, !local_); desktop_ = desktop;
    terminal_.flush();
}
void Session::poll(int timeout_ms) {
    if (Terminal::resized()) {
        const auto cells = terminal_.cells();
        if (cells.width > 0 && cells.height > 0) { viewport_ = cells; renderer_.viewport(cells); }
        pixels_ = terminal_.pixels(); display_dirty_ = true;
        display_changed_ = std::chrono::steady_clock::now();
        terminal_.write("\033[16t\033[18t\033[14t"); terminal_.flush();
    }
    const auto bytes = terminal_.read(timeout_ms);
    const bool had_escape = input_.pending_escape();
    if (!bytes.empty()) input_.feed(bytes);
    // Apply one resize after the complete batch of geometry replies. The server's
    // framebuffer acknowledgement, rather than the request, reconfigures rendering.
    if (local_ && display_dirty_ && backend_ && desktop_.width > 0 &&
        std::chrono::steady_clock::now() - display_changed_ >= std::chrono::milliseconds(50)) {
        const auto desired = display_size();
        if (desired != pending_display_) { backend_->resize_desktop(desired); pending_display_ = desired; }
        display_dirty_ = false;
    }
    const auto now = std::chrono::steady_clock::now();
    if (input_.pending_escape()) {
        if (!had_escape) escape_since_ = now;
        else if (now - escape_since_ > std::chrono::milliseconds(40)) input_.flush_escape();
    }
    check_error();
}
Size Session::display_size() const {
    int64_t width = pixels_.width, height = pixels_.height;
    if (width <= 0 || height <= 0) {
        if (viewport_.width <= 0 || viewport_.height <= 0)
            throw std::runtime_error("terminal did not report drawable dimensions");
        width = static_cast<int64_t>(viewport_.width) * cell_.width;
        height = static_cast<int64_t>(viewport_.height) * cell_.height;
    }
    width = std::max<int64_t>(1, width / magnification_);
    height = std::max<int64_t>(1, height / magnification_);
    if (width > INT_MAX || height > INT_MAX) throw std::runtime_error("terminal pixel dimensions overflow");
    const Size result{static_cast<int>(width), static_cast<int>(height)};
    validate_desktop(result);
    return result;
}
void Session::paint() {
    check_error();
    if (backend_ && desktop_.width > 0) {
        const auto frame = backend_->frame();
        // A VNC allocation/resize can precede the first pixel update. Do not
        // flood the terminal with a full desktop of uninitialized black tiles.
        if (frame.data) renderer_.paint(frame);
        terminal_.flush();
    }
}
void Session::event(const Event& value) {
    switch (value.type) {
    case EventType::quit: quit_ = true; break;
    case EventType::cell_size:
        if (!fixed_cell_ && value.size.width > 0 && value.size.height > 0 &&
            value.size.width <= 512 && value.size.height <= 512 && value.size != cell_) {
            cell_ = value.size;
            display_dirty_ = true;
            display_changed_ = std::chrono::steady_clock::now();
            if (local_) { terminal_.write("\033[18t\033[14t"); terminal_.flush(); }
            if (desktop_.width) resize(desktop_);
        }
        break;
    case EventType::terminal_size:
        if (value.size != viewport_) {
            pixels_ = {}; display_dirty_ = true; display_changed_ = std::chrono::steady_clock::now();
        }
        viewport_ = value.size; renderer_.viewport(viewport_); break;
    case EventType::pixel_size:
        pixels_ = value.size; display_dirty_ = true;
        display_changed_ = std::chrono::steady_clock::now();
        if (!fixed_cell_ && cell_.width == 0 && viewport_.width > 0 && viewport_.height > 0)
            cell_ = {value.size.width / viewport_.width, value.size.height / viewport_.height};
        break;
    case EventType::mouse_mode:
        if (value.action == 1 || value.action == 2) {
            pixel_mouse_ = true; terminal_.write("\033[?1016h"); terminal_.flush();
        }
        break;
    case EventType::key: if (backend_ && !view_only_) backend_->key(value); break;
    case EventType::mouse: {
        if (!backend_ || view_only_ || !desktop_.width) break;
        int64_t x = value.x, y = value.y;
        if (!pixel_mouse_) { x = x * cell_.width + cell_.width / 2; y = y * cell_.height + cell_.height / 2; }
        x /= magnification_; y /= magnification_;
        x = std::clamp<int64_t>(x, 0, desktop_.width - 1);
        y = std::clamp<int64_t>(y, 0, desktop_.height - 1);
        const int button = value.button & 3;
        // Extended buttons are not supported; never turn them into left clicks.
        if (value.button & ~127) break;
        int wheel = 0;
        if (value.button & 64) {
            static constexpr int directions[] = {1, -1, -2, 2};
            if (value.action != 3) wheel = directions[button];
        } else if (!(value.button & 32)) {
            if (button < 3) {
                if (value.action == 3) mouse_mask_ &= ~(1 << button);
                else mouse_mask_ |= 1 << button;
            } else mouse_mask_ = 0;
        }
        backend_->pointer(static_cast<int>(x), static_cast<int>(y), mouse_mask_, wheel, value.modifiers);
        break;
    }
    }
}
}
