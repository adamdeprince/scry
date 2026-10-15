// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Adam DePrince
#pragma once
#include "input.hpp"
#include "terminal.hpp"
#include <chrono>
#include <exception>
#include <memory>

namespace goblin {
struct Options {
    std::string protocol, host, username, password, domain;
    std::string hdr = "auto";
    std::vector<std::string> command;
    int port = 0;
    int magnification = 1;
    Size desktop{1280, 720}, cell;
    size_t max_tiles = 0;
    bool view_only = false, insecure = false;
};
class Session;
class Backend {
public:
    virtual ~Backend() = default;
    virtual void connect(Session& session, const Options& options) = 0;
    virtual bool step(int timeout_ms) = 0;
    virtual Frame frame() const = 0;
    virtual void key(const Event& event) = 0;
    // mask: left=1, middle=2, right=4; wheel: up=1, down=-1, right=2, left=-2.
    virtual void pointer(int x, int y, int mask, int wheel, unsigned modifiers) = 0;
    virtual void resize_desktop(Size) {}
};
std::unique_ptr<Backend> make_rdp();
std::unique_ptr<Backend> make_hpss();
// A connected socket can be supplied by an embedding launcher (ownership transfers).
std::unique_ptr<Backend> make_vnc(int connected_socket = -1);
std::unique_ptr<Backend> make_local(const std::string& protocol);

class Session {
public:
    Session(Terminal& terminal, const Options& options);
    ~Session();
    void attach(Backend* backend) { backend_ = backend; }
    void resize(Size desktop);
    void damage(int x, int y, int width, int height) { renderer_.damage(x, y, width, height); }
    void poll(int timeout_ms = 0);
    void paint();
    bool stopping() const { return quit_ || Terminal::stopping(); }
    void fail(std::exception_ptr error) { if (!error_) error_ = error; }
    void check_error() const { if (error_) std::rethrow_exception(error_); }
    Renderer& renderer() { return renderer_; }
    Size display_size() const;
    bool hdr_supported() const { return hdr_supported_; }
private:
    void event(const Event& event);
    Terminal& terminal_;
    Renderer renderer_;
    Input input_;
    Backend* backend_ = nullptr;
    Size cell_, viewport_, desktop_, pixels_, pending_display_;
    bool local_ = false, display_dirty_ = false;
    bool fixed_cell_ = false, view_only_ = false, pixel_mouse_ = false, quit_ = false;
    bool hdr_probe_ = false, hdr_supported_ = false;
    int mouse_mask_ = 0;
    int magnification_ = 1;
    std::exception_ptr error_;
    std::chrono::steady_clock::time_point escape_since_{};
    std::chrono::steady_clock::time_point display_changed_{};
};
}
