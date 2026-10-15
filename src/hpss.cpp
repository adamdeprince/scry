// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Adam DePrince
#include "session.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
extern char** environ;
#ifndef GOBLIN_HPSS_HELPER
#define GOBLIN_HPSS_HELPER "scripts/hpss.py"
#endif
namespace goblin {
namespace {
void put_number(std::string& out, uint64_t n, size_t width) {
    for (size_t i = width; i; --i) out += char(n >> ((i - 1) * 8));
}
uint64_t get_number(std::string_view in, size_t offset, size_t width) {
    if (offset > in.size() || width > in.size() - offset) throw std::runtime_error("truncated HPSS helper packet");
    uint64_t n = 0;
    for (size_t i = 0; i < width; ++i) n = (n << 8) | uint8_t(in[offset + i]);
    return n;
}
int dimension(std::string_view in, size_t offset) {
    const auto n = get_number(in, offset, 4);
    if (n > INT_MAX) throw std::runtime_error("HPSS dimension overflow");
    return int(n);
}
std::filesystem::path executable() {
#ifdef __APPLE__
    uint32_t length = 0;
    _NSGetExecutablePath(nullptr, &length);
    std::vector<char> path(length);
    if (_NSGetExecutablePath(path.data(), &length)) throw std::runtime_error("cannot locate HPSS runtime");
    return std::filesystem::canonical(path.data());
#else
    std::vector<char> path(256);
    for (;;) {
        auto length = readlink("/proc/self/exe", path.data(), path.size());
        if (length < 0) throw std::runtime_error("cannot locate HPSS runtime");
        if (size_t(length) < path.size()) return std::string(path.data(), size_t(length));
        if (path.size() > path.max_size() / 2) throw std::length_error("executable path overflow");
        path.resize(path.size() * 2);
    }
#endif
}
class Hpss final : public Backend {
public:
    ~Hpss() override {
        if (socket_ >= 0) {
            try { queue("Q"); flush(); } catch (...) {}
            close(socket_);
        }
        if (child_ > 0) {
            // Give the helper time to release input and close its media threads.
            const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            int status = 0;
            while (waitpid(child_, &status, WNOHANG) == 0) {
                if (std::chrono::steady_clock::now() >= end) {
                    kill(child_, SIGKILL);
                    while (waitpid(child_, &status, 0) < 0 && errno == EINTR) {}
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
    }
    void connect(Session& session, const Options& options) override {
        owner_ = &session;
        const auto installed = executable().parent_path().parent_path() / "libexec/goblin-scry";
        std::string helper = (installed / "hpss.py").string();
        if (const char* value = getenv("GOBLIN_HPSS_HELPER")) helper = value;
        else if (!std::filesystem::is_regular_file(helper)) helper = GOBLIN_HPSS_HELPER;
        if (!std::filesystem::is_regular_file(helper)) throw std::runtime_error("HPSS helper is missing from this installation");
        std::string python = "python3";
        if (const char* value = getenv("GOBLIN_HPSS_PYTHON")) python = value;
        else if (std::filesystem::is_regular_file(installed / "hpss-venv/bin/python3"))
            python = (installed / "hpss-venv/bin/python3").string();
        int sockets[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets)) throw std::runtime_error("cannot create HPSS helper socket");
        posix_spawn_file_actions_t actions;
        int error = posix_spawn_file_actions_init(&actions);
        const bool actions_ready = error == 0;
        if (!error) error = posix_spawn_file_actions_adddup2(&actions, sockets[1], STDIN_FILENO);
        if (!error) error = posix_spawn_file_actions_adddup2(&actions, sockets[1], STDOUT_FILENO);
        if (!error) error = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
        if (!error) error = posix_spawn_file_actions_addclose(&actions, sockets[0]);
        if (!error) error = posix_spawn_file_actions_addclose(&actions, sockets[1]);
        // Credentials go over the private socket, never argv or a child environment.
        std::vector<std::string> environment;
        for (char** item = environ; *item; ++item)
            if (std::string_view(*item).substr(0, 16) != "GOBLIN_PASSWORD=") environment.emplace_back(*item);
        std::vector<char*> env;
        for (auto& value : environment) env.push_back(value.data());
        env.push_back(nullptr);
        char* argv[] = {python.data(), helper.data(), nullptr};
        if (!error) error = posix_spawnp(&child_, python.c_str(), &actions, nullptr, argv, env.data());
        if (actions_ready) posix_spawn_file_actions_destroy(&actions);
        close(sockets[1]);
        if (error) { close(sockets[0]); child_ = -1; throw std::runtime_error("cannot start HPSS Python runtime: " + std::string(strerror(error))); }
        socket_ = sockets[0];
        const int flags = fcntl(socket_, F_GETFL);
        if (flags < 0 || fcntl(socket_, F_SETFL, flags | O_NONBLOCK) < 0) throw std::runtime_error("cannot configure HPSS socket");
        std::string config = "C";
        for (auto n : {1, options.port ? options.port : 5900, options.desktop.width, options.desktop.height, session.hdr_supported() ? 1 : 0}) put_number(config, unsigned(n), 4);
        for (const auto* value : {&options.host, &options.username, &options.password}) { put_number(config, value->size(), 8); config += *value; }
        queue(config);
        while (!ready_ && !session.stopping()) { step(20); session.poll(); }
    }
    bool step(int timeout_ms) override {
        pollfd descriptor{socket_, short(POLLIN | (outgoing_.empty() ? 0 : POLLOUT)), 0};
        const int result = ::poll(&descriptor, 1, timeout_ms);
        if (result < 0 && errno != EINTR) throw std::runtime_error("HPSS helper poll failed");
        if (result <= 0) return true;
        if (descriptor.revents & POLLOUT) flush();
        if (descriptor.revents & (POLLIN | POLLHUP)) {
            // Streaming chunk, not a message or framebuffer size limit.
            char chunk[16384];
            const auto count = recv(socket_, chunk, sizeof chunk, 0);
            if (count == 0) throw std::runtime_error("HPSS helper disconnected");
            if (count < 0 && errno != EINTR && errno != EAGAIN) throw std::runtime_error("HPSS helper read failed");
            if (count > 0) incoming_.append(chunk, size_t(count));
            size_t consumed = 0;
            while (incoming_.size() - consumed >= 8) {
                const auto length = get_number(incoming_, consumed, 8);
                if (!length || length > incoming_.max_size() - 8) throw std::runtime_error("invalid HPSS helper packet length");
                if (length > incoming_.size() - consumed - 8) break;
                packet(std::string_view(incoming_).substr(consumed + 8, size_t(length)));
                consumed += 8 + size_t(length);
            }
            incoming_.erase(0, consumed);
        }
        if (descriptor.revents & (POLLERR | POLLNVAL)) throw std::runtime_error("HPSS helper socket failed");
        return true;
    }
    Frame frame() const override { return {have_frame_ ? pixels_.data() : nullptr, size_, size_t(size_.width) * 8, 0, 1, 2, PixelFormat::linear_rgba16}; }
    void key(const Event& event) override {
        std::string message = "K";
        for (auto n : {event.key, event.physical, event.modifiers, unsigned(event.action)}) put_number(message, n, 4);
        queue(message);
    }
    void pointer(int x, int y, int mask, int wheel, unsigned modifiers) override {
        std::string message = "P";
        for (auto n : {unsigned(x), unsigned(y), unsigned(mask), unsigned(wheel), modifiers}) put_number(message, n, 4);
        queue(message); owner_->renderer().cursor_position(x, y);
    }
private:
    void queue(std::string_view data) { put_number(outgoing_, data.size(), 8); outgoing_.append(data); }
    void flush() {
        while (!outgoing_.empty()) {
            const auto count = send(socket_, outgoing_.data(), outgoing_.size(), 0);
            if (count < 0 && errno == EINTR) continue;
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
            if (count <= 0) throw std::runtime_error("HPSS helper write failed");
            outgoing_.erase(0, size_t(count));
        }
    }
    void resize(Size size) {
        if (size == size_) return;
        validate_desktop(size);
        const auto width = size_t(size.width), height = size_t(size.height);
        if (width > pixels_.max_size() / 8 / height) throw std::length_error("HPSS framebuffer allocation overflow");
        std::vector<uint8_t> pixels(width * height * 8, 0);
        for (size_t i = 0; i < pixels.size(); i += 8) pixels[i + 7] = 0x3c; // Opaque black binary16.
        owner_->resize(size);
        pixels_ = std::move(pixels); size_ = size; have_frame_ = false;
    }
    void packet(std::string_view data) {
        if (data[0] == 'E') throw std::runtime_error("HPSS: " + std::string(data.substr(1)));
        if (data[0] == 'R' && data.size() == 9) {
            resize({dimension(data, 1), dimension(data, 5)}); ready_ = true;
        } else if (data[0] == 'F' && data.size() >= 25) {
            const int x = dimension(data, 1), y = dimension(data, 5), w = dimension(data, 9), h = dimension(data, 13);
            const Size canvas{dimension(data, 17), dimension(data, 21)};
            validate_desktop(canvas);
            if (!ready_ || w <= 0 || h <= 0 || x > canvas.width || y > canvas.height ||
                w > canvas.width - x || h > canvas.height - y || size_t(w) > (data.size() - 25) / 8 / size_t(h) ||
                size_t(w) * size_t(h) * 8 != data.size() - 25) throw std::runtime_error("invalid HPSS frame geometry");
            // Reject non-finite binary16 data before committing any pixels.
            for (size_t i = 25; i < data.size(); i += 2) half_read(reinterpret_cast<const uint8_t*>(data.data() + i));
            resize(canvas);
            for (int row = 0; row < h; ++row)
                memcpy(pixels_.data() + (size_t(y + row) * size_.width + x) * 8,
                       data.data() + 25 + size_t(row) * w * 8, size_t(w) * 8);
            have_frame_ = true; owner_->damage(x, y, w, h);
        } else if (data[0] == 'U' && data.size() >= 17) {
            owner_->renderer().cursor_shape({dimension(data, 1), dimension(data, 5)},
                {dimension(data, 9), dimension(data, 13)}, std::string(data.substr(17)));
        } else throw std::runtime_error("unknown HPSS helper packet");
    }
    Session* owner_ = nullptr;
    int socket_ = -1; pid_t child_ = -1;
    bool ready_ = false, have_frame_ = false;
    Size size_;
    std::vector<uint8_t> pixels_;
    std::string incoming_, outgoing_;
};
}
std::unique_ptr<Backend> make_hpss() { return std::make_unique<Hpss>(); }
}
