// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Adam DePrince
#include "session.hpp"
#include <charconv>
#include <climits>
#include <cstdlib>
#include <csignal>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

namespace goblin {
namespace {
int integer(std::string_view text, int maximum) {
    int result = 0; const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || result <= 0 || result > maximum)
        throw std::runtime_error("invalid positive integer: " + std::string(text));
    return result;
}
Size size(std::string_view value, int maximum) {
    const auto x = value.find('x');
    if (x == std::string_view::npos) throw std::runtime_error("expected WIDTHxHEIGHT");
    return {integer(value.substr(0, x), maximum), integer(value.substr(x + 1), maximum)};
}
void help() {
    std::cout << "Goblin Scry " GOBLIN_SCRY_VERSION " — graphical desktops in your terminal\n"
        "Copyright (C) 2026 Adam DePrince. Licensed under GPLv3; no warranty.\n\n"
        "Usage: goblin-scry rdp|vnc HOST [options]\n"
        "       goblin-scry demo [options]\n\n"
        "       goblin-scry x11|wayland [options] [-- COMMAND [ARG...]]\n\n"
        "  --username USER       RDP / Apple ARD account\n"
        "  --domain DOMAIN       RDP domain\n"
        "  --port PORT           TCP port (RDP 3389, VNC 5900)\n"
        "  --size WIDTHxHEIGHT   Requested RDP / demo pixels (1280x720)\n"
        "  --magnification N    Enlarge pixels by a positive integer factor (1)\n"
        "  --cell-size WxH       Override terminal cell pixel measurement\n"
        "  --max-tiles COUNT     Terminal image budget (262144)\n"
        "  --view-only           Disable remote keyboard and mouse input\n"
        "  --insecure            Disable RDP certificate verification\n\n"
        "Password: GOBLIN_PASSWORD environment variable, or a hidden prompt.\n"
        "Exit: Ctrl+] (Ctrl+C is sent to the remote desktop).\n"
        "Backends:";
#ifdef GOBLIN_WITH_RDP
    std::cout << " FreeRDP3";
#endif
#ifdef GOBLIN_WITH_VNC
    std::cout << " LibVNCClient (Apple ARD)";
#endif
    std::cout << " demo\n"
        "Local modes: x11 (TigerVNC + xrandr), wayland (Sway + wayvnc).\n"
        "Local desktops fill the terminal and follow its pixel resolution.\n"
        "Use -- COMMAND to launch an application inside the local display.\n";
}
class Demo final : public Backend {
public:
    void connect(Session& session, const Options& options) override {
        owner_ = &session; size_ = options.desktop;
        pixels_.resize(static_cast<size_t>(size_.width) * size_.height * 4);
        for (int y = 0; y < size_.height; ++y) for (int x = 0; x < size_.width; ++x) {
            const size_t index = (static_cast<size_t>(y) * size_.width + x) * 4;
            pixels_[index] = static_cast<uint8_t>(x * 255 / size_.width);
            pixels_[index + 1] = static_cast<uint8_t>(y * 255 / size_.height);
            pixels_[index + 2] = 80;
        }
        session.resize(size_); last_ = std::chrono::steady_clock::now();
    }
    bool step(int) override {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_ >= std::chrono::milliseconds(300)) {
            pixels_[0] ^= 255; owner_->damage(0, 0, 1, 1); last_ = now;
        }
        return true;
    }
    Frame frame() const override { return {pixels_.data(), size_, static_cast<size_t>(size_.width) * 4, 0, 1, 2}; }
    void key(const Event&) override {}
    void pointer(int, int, int, int, unsigned) override {}
private:
    Session* owner_ = nullptr; Size size_; std::vector<uint8_t> pixels_;
    std::chrono::steady_clock::time_point last_;
};
std::string prompt_password() {
    if (!isatty(STDIN_FILENO)) throw std::runtime_error("password prompt requires a TTY; set GOBLIN_PASSWORD");
    // A caught termination signal interrupts getline so stack unwinding restores echo.
    struct PromptMode {
        termios saved{};
        struct sigaction previous[3]{};
        int signals[3]{SIGINT, SIGTERM, SIGHUP};
        PromptMode() {
            if (tcgetattr(STDIN_FILENO, &saved) != 0) throw std::runtime_error("cannot read password terminal mode");
            struct sigaction action{};
            action.sa_handler = [](int) {}; sigemptyset(&action.sa_mask);
            for (int i = 0; i < 3; ++i) sigaction(signals[i], &action, &previous[i]);
            auto hidden = saved; hidden.c_lflag &= ~ECHO;
            if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &hidden) != 0) {
                for (int i = 0; i < 3; ++i) sigaction(signals[i], &previous[i], nullptr);
                throw std::runtime_error("cannot hide password input");
            }
        }
        ~PromptMode() {
            tcsetattr(STDIN_FILENO, TCSANOW, &saved);
            for (int i = 0; i < 3; ++i) sigaction(signals[i], &previous[i], nullptr);
        }
    } mode;
    std::cerr << "Password: " << std::flush;
    std::string password; std::getline(std::cin, password);
    std::cerr << '\n';
    if (!std::cin) throw std::runtime_error("cannot read password; set GOBLIN_PASSWORD");
    return password;
}
}
}
int main(int argc, char** argv) {
    using namespace goblin;
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--version") {
            std::cout << "goblin-scry " GOBLIN_SCRY_VERSION "\n";
            return 0;
        }
        if (argc < 2 || std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h") {
            help(); return argc < 2 ? 1 : 0;
        }
        Options options; options.protocol = argv[1];
        const bool local = options.protocol == "x11" || options.protocol == "wayland";
        if (options.protocol != "rdp" && options.protocol != "vnc" && options.protocol != "demo" && !local)
            throw std::runtime_error("protocol must be rdp, vnc, x11, wayland, or demo");
        int index = 2;
        if (options.protocol == "rdp" || options.protocol == "vnc") {
            if (index >= argc || argv[index][0] == '-') throw std::runtime_error("missing server hostname");
            options.host = argv[index++];
        }
        while (index < argc) {
            const std::string_view arg = argv[index++];
            if (arg == "--help") { help(); return 0; }
            if (arg == "--") {
                if (!local || index == argc) throw std::runtime_error("-- COMMAND requires x11 or wayland mode");
                while (index < argc) options.command.emplace_back(argv[index++]);
                break;
            }
            if (arg == "--view-only") { options.view_only = true; continue; }
            if (arg == "--insecure") { options.insecure = true; continue; }
            if (index >= argc) throw std::runtime_error("missing value for " + std::string(arg));
            const std::string_view value = argv[index++];
            if (arg == "--username") options.username = value;
            else if (arg == "--domain") options.domain = value;
            else if (arg == "--port") {
                if (local) throw std::runtime_error("local display modes use a private Unix socket; --port is not supported");
                options.port = integer(value, 65535);
            }
            else if (arg == "--size") {
                if (local) throw std::runtime_error("local display modes use the full terminal resolution; --size is not supported");
                options.desktop = size(value, 32768);
            }
            else if (arg == "--magnification") options.magnification = integer(value, INT_MAX);
            else if (arg == "--cell-size") options.cell = size(value, 512);
            else if (arg == "--max-tiles") options.max_tiles = integer(value, 262144);
            else throw std::runtime_error("unknown option: " + std::string(arg));
        }
        validate_desktop(options.desktop);
        if (!local && options.protocol != "vnc") grid_size(options.desktop, {512, 512}, options.magnification);
        auto pending_backend = options.protocol == "demo" ? std::unique_ptr<Backend>(std::make_unique<Demo>()) :
                       local ? make_local(options.protocol) : options.protocol == "rdp" ? make_rdp() : make_vnc();
        if (options.protocol == "rdp" || options.protocol == "vnc") {
            if (const char* password = getenv("GOBLIN_PASSWORD")) options.password = password;
            else options.password = prompt_password();
        }
        Terminal terminal;
        Session session(terminal, options);
        auto backend = std::move(pending_backend);
        session.attach(backend.get());
        if (session.stopping()) return 0;
        backend->connect(session, options);
        if (session.stopping()) return 0;
        session.paint();
        while (!session.stopping()) {
            session.poll(options.protocol == "demo" ? 20 : 0);
            if (session.stopping() || !backend->step(20)) break;
            session.paint();
        }
        session.attach(nullptr);
        // Free protocol objects while their callback context and terminal still exist.
        backend.reset();
        return 0;
    } catch (const std::exception& error) {
        // Server-provided error text cannot inject terminal control sequences.
        std::string message = error.what();
        for (char& c : message) if (static_cast<unsigned char>(c) < 32 || c == 127) c = ' ';
        std::cerr << "goblin-scry: " << message << '\n'; return 1;
    }
}
