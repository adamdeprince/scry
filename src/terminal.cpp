// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Adam DePrince
#include "terminal.hpp"
#include <cerrno>
#include <csignal>
#include <poll.h>
#include <stdexcept>
#include <sys/ioctl.h>
#include <unistd.h>

namespace goblin {
namespace {
volatile sig_atomic_t stop_requested = 0, resize_requested = 0;
void signal_handler(int signal) {
    if (signal == SIGWINCH) resize_requested = 1;
    else stop_requested = 1;
}
void write_all(std::string_view bytes) {
    while (!bytes.empty()) {
        const auto count = ::write(STDOUT_FILENO, bytes.data(), bytes.size());
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw std::runtime_error("terminal write failed");
        bytes.remove_prefix(static_cast<size_t>(count));
    }
}
winsize window_size() {
    winsize size{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) != 0) throw std::runtime_error("cannot query terminal size");
    return size;
}
}
Terminal::Terminal() {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
        throw std::runtime_error("run inside a Kitty graphics terminal (stdin and stdout must be a TTY)");
    if (tcgetattr(STDIN_FILENO, &saved_) != 0) throw std::runtime_error("cannot read terminal mode");
    auto mode = saved_; cfmakeraw(&mode);
    mode.c_cc[VMIN] = 0; mode.c_cc[VTIME] = 0;
    struct sigaction action{};
    action.sa_handler = signal_handler; action.sa_flags = SA_RESTART; sigemptyset(&action.sa_mask);
    for (int signal : {SIGINT, SIGTERM, SIGHUP, SIGWINCH}) sigaction(signal, &action, nullptr);
    signal(SIGPIPE, SIG_IGN);
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &mode) != 0) throw std::runtime_error("cannot enter terminal raw mode");
    raw_ = true;
    try {
        active_ = true;
        write_all("\033[?1049h\033[?25l\033[2J\033[H\033[>15u\033[?1003h\033[?1006h\033[?2048h"
                  "\033[?1016$p\033[16t\033[18t\033[14t");
    } catch (...) {
        tcsetattr(STDIN_FILENO, TCSANOW, &saved_); raw_ = false; throw;
    }
}
Terminal::~Terminal() {
    if (active_) {
        try {
            flush();
            write_all("\033[?2026l\033[?1016l\033[?1003l\033[?1006l\033[?2048l\033[<u\033[?25h\033[?1049l");
        } catch (...) {}
    }
    if (raw_) tcsetattr(STDIN_FILENO, TCSANOW, &saved_);
}
void Terminal::write(std::string_view bytes) {
    output_.append(bytes);
    if (output_.size() >= 65536) flush();
}
void Terminal::flush() {
    if (!output_.empty()) { write_all(output_); output_.clear(); }
}
std::string Terminal::read(int timeout_ms) {
    pollfd fd{STDIN_FILENO, POLLIN, 0};
    const int result = poll(&fd, 1, timeout_ms);
    if (result < 0 && errno != EINTR) throw std::runtime_error("terminal poll failed");
    if (result <= 0) return {};
    if (fd.revents & (POLLERR | POLLHUP | POLLNVAL)) { stop_requested = 1; return {}; }
    char bytes[8192]; const auto count = ::read(STDIN_FILENO, bytes, sizeof bytes);
    if (count < 0 && errno != EINTR && errno != EAGAIN) throw std::runtime_error("terminal read failed");
    return count > 0 ? std::string(bytes, static_cast<size_t>(count)) : std::string{};
}
Size Terminal::cells() const { const auto size = window_size(); return {size.ws_col, size.ws_row}; }
Size Terminal::cell_pixels() const {
    const auto size = window_size();
    return {size.ws_col ? size.ws_xpixel / size.ws_col : 0, size.ws_row ? size.ws_ypixel / size.ws_row : 0};
}
Size Terminal::pixels() const { const auto size = window_size(); return {size.ws_xpixel, size.ws_ypixel}; }
bool Terminal::stopping() { return stop_requested != 0; }
bool Terminal::resized() { const bool result = resize_requested; resize_requested = 0; return result; }
}
