// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Adam DePrince
#include "session.hpp"
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <spawn.h>
#include <stdexcept>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace goblin {
namespace {
using Environment = std::map<std::string, std::string>;
using Clock = std::chrono::steady_clock;

class Descriptor {
public:
    explicit Descriptor(int fd = -1) : fd_(fd) {}
    ~Descriptor() { if (fd_ >= 0) close(fd_); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    int get() const { return fd_; }
    int release() { const int result = fd_; fd_ = -1; return result; }
private:
    int fd_;
};
class Directory {
public:
    Directory() {
        char path[] = "/tmp/goblin-scry-XXXXXX";
        if (!mkdtemp(path)) throw std::runtime_error("cannot create local display directory");
        path_ = path;
    }
    ~Directory() { std::error_code error; std::filesystem::remove_all(path_, error); }
    std::string file(std::string_view name) const { return path_ + "/" + std::string(name); }
    const std::string& path() const { return path_; }
private:
    std::string path_;
};
std::string executable(std::initializer_list<const char*> names) {
    const char* search = getenv("PATH");
    for (const auto* name : names) {
        std::string_view paths = search ? search : "/usr/local/bin:/usr/bin:/bin";
        while (true) {
            const auto end = paths.find(':');
            const auto directory = paths.substr(0, end);
            const auto path = std::string(directory.empty() ? "." : directory) + "/" + name;
            if (access(path.c_str(), X_OK) == 0 && !std::filesystem::is_directory(path)) return path;
            if (end == std::string_view::npos) break;
            paths.remove_prefix(end + 1);
        }
    }
    throw std::runtime_error("local display mode requires " + std::string(*names.begin()) + " in PATH");
}
void file(const std::string& path, const std::string& data) {
    std::ofstream output(path, std::ios::binary);
    output.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!output) throw std::runtime_error("cannot write local display configuration");
}
std::string log_tail(const std::string& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return {};
    const auto length = input.tellg();
    input.seekg(std::max<std::streamoff>(0, static_cast<std::streamoff>(length) - 2048));
    return std::string(std::istreambuf_iterator<char>(input), {});
}

// Each child has its own process group, detached from terminal input/output.
// This also lets cleanup stop children of a launched desktop command.
class Child {
public:
    Child(const std::vector<std::string>& command, const Environment& environment,
          const std::string& log, int display_fd = -1) : name_(command.at(0)), log_(log) {
        std::vector<std::string> variables;
        for (auto** entry = environ; *entry; ++entry) {
            const std::string value = *entry;
            if (!environment.count(value.substr(0, value.find('=')))) variables.push_back(value);
        }
        for (const auto& entry : environment)
            if (!entry.second.empty()) variables.push_back(entry.first + "=" + entry.second);
        std::vector<char*> arguments, env;
        for (const auto& arg : command) arguments.push_back(const_cast<char*>(arg.c_str()));
        arguments.push_back(nullptr);
        for (auto& value : variables) env.push_back(value.data());
        env.push_back(nullptr);
        posix_spawn_file_actions_t actions;
        posix_spawnattr_t attributes;
        if (posix_spawn_file_actions_init(&actions) != 0) throw std::runtime_error("cannot initialize child actions");
        if (posix_spawnattr_init(&attributes) != 0) {
            posix_spawn_file_actions_destroy(&actions);
            throw std::runtime_error("cannot initialize child attributes");
        }
        int error = posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
        if (!error) error = posix_spawn_file_actions_addopen(&actions, 1, log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (!error) error = posix_spawn_file_actions_adddup2(&actions, 1, 2);
        if (!error && display_fd >= 0) error = posix_spawn_file_actions_adddup2(&actions, display_fd, 3);
        sigset_t defaults, mask;
        sigemptyset(&defaults); sigemptyset(&mask);
        for (int signal : {SIGINT, SIGTERM, SIGHUP, SIGPIPE, SIGWINCH}) sigaddset(&defaults, signal);
        if (!error) error = posix_spawnattr_setsigdefault(&attributes, &defaults);
        if (!error) error = posix_spawnattr_setsigmask(&attributes, &mask);
        if (!error) error = posix_spawnattr_setpgroup(&attributes, 0);
        if (!error) error = posix_spawnattr_setflags(&attributes,
            POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);
        if (!error) error = posix_spawnp(&pid_, arguments[0], &actions, &attributes, arguments.data(), env.data());
        posix_spawnattr_destroy(&attributes); posix_spawn_file_actions_destroy(&actions);
        if (error) throw std::runtime_error("cannot start " + name_ + ": " + strerror(error));
    }
    ~Child() {
        if (pid_ <= 0) return;
        kill(-pid_, SIGTERM);
        const auto deadline = Clock::now() + std::chrono::milliseconds(500);
        while (!reaped_ && Clock::now() < deadline) {
            if (waitpid(pid_, &status_, WNOHANG) == pid_) reaped_ = true;
            else usleep(10000);
        }
        kill(-pid_, SIGKILL);
        if (!reaped_) while (waitpid(pid_, &status_, 0) < 0 && errno == EINTR) {}
    }
    bool running() {
        if (!reaped_) {
            const auto result = waitpid(pid_, &status_, WNOHANG);
            if (result < 0 && errno != EINTR) throw std::runtime_error("cannot wait for " + name_);
            if (result == pid_) reaped_ = true;
        }
        return !reaped_;
    }
    bool success() const { return reaped_ && WIFEXITED(status_) && WEXITSTATUS(status_) == 0; }
    std::runtime_error failure(const std::string& reason = "exited") const {
        return std::runtime_error(name_ + " " + reason + ": " + log_tail(log_));
    }
private:
    pid_t pid_ = -1;
    int status_ = 0;
    bool reaped_ = false;
    std::string name_, log_;
};
std::string dimensions(Size size) { return std::to_string(size.width) + "x" + std::to_string(size.height); }
int unix_connection(const std::string& path) {
    sockaddr_un address{}; address.sun_family = AF_UNIX;
    if (path.size() >= sizeof address.sun_path) throw std::runtime_error("local display socket path is too long");
    memcpy(address.sun_path, path.c_str(), path.size() + 1);
    Descriptor socket(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (socket.get() < 0) throw std::runtime_error("cannot create local display socket");
    if (fcntl(socket.get(), F_SETFD, FD_CLOEXEC) < 0) throw std::runtime_error("cannot protect local display socket");
    if (::connect(socket.get(), reinterpret_cast<sockaddr*>(&address), sizeof address) == 0) return socket.release();
    if (errno != ENOENT && errno != ECONNREFUSED && errno != EINTR)
        throw std::runtime_error("cannot connect to local display socket: " + std::string(strerror(errno)));
    return -1;
}

class Local final : public Backend {
public:
    explicit Local(std::string protocol) : protocol_(std::move(protocol)) {
        // Check dependencies before changing terminal modes.
        if (protocol_ == "x11") { server_path_ = executable({"Xvnc", "Xtigervnc"}); control_path_ = executable({"xrandr"}); }
        else { server_path_ = executable({"sway"}); control_path_ = executable({"swaymsg"}); bridge_path_ = executable({"wayvnc"}); }
    }
    ~Local() override { vnc_.reset(); application_.reset(); bridge_.reset(); server_.reset(); }
    void connect(Session& owner, const Options& options) override {
        owner_ = &owner;
        const auto size = owner.display_size();
        validate_size(size);
        env_ = {{"DISPLAY", ""}, {"WAYLAND_DISPLAY", ""}, {"SWAYSOCK", ""}, {"XAUTHORITY", ""},
                {"GOBLIN_PASSWORD", ""}, {"XDG_SESSION_TYPE", protocol_ == "x11" ? "x11" : "wayland"}};
        if (protocol_ == "x11") start_x11(size);
        else start_wayland(size);
        if (owner.stopping()) return;
        int connected = -1;
        if (!wait([&] { connected = unix_connection(directory_.file("rfb")); return connected >= 0; })) return;
        vnc_ = make_vnc(connected);
        Options connection = options; connection.host = "local-" + protocol_; connection.password.clear(); connection.username.clear();
        vnc_->connect(owner, connection);
        if (!options.command.empty())
            application_ = std::make_unique<Child>(options.command, env_, directory_.file("application.log"));
    }
    bool step(int timeout_ms) override {
        check_servers();
        if (application_ && !application_->running()) {
            if (!application_->success()) throw application_->failure();
            return false;
        }
        bool result;
        try { result = vnc_->step(timeout_ms); }
        catch (const std::exception& error) {
            throw std::runtime_error(std::string(error.what()) + "\nLocal server: " + log_tail(directory_.file("server.log")) +
                (bridge_ ? "\nwayvnc: " + log_tail(directory_.file("wayvnc.log")) : ""));
        }
        if (expected_.width && vnc_->frame().size == expected_) expected_ = {};
        if (expected_.width && Clock::now() > resize_deadline_)
            throw std::runtime_error("local " + protocol_ + " server did not resize to " + dimensions(expected_));
        return result;
    }
    Frame frame() const override { return vnc_->frame(); }
    void key(const Event& event) override { if (vnc_) vnc_->key(event); }
    void pointer(int x, int y, int mask, int wheel, unsigned modifiers) override {
        if (vnc_) vnc_->pointer(x, y, mask, wheel, modifiers);
    }
    void resize_desktop(Size size) override {
        validate_size(size);
        if (protocol_ == "wayland") run({control_path_, "-s", env_.at("SWAYSOCK"), "output HEADLESS-1 mode --custom " + dimensions(size)});
        else {
            const auto name = "goblin-" + dimensions(size);
            // TigerVNC's RandR output accepts custom modes. A complete modeline
            // avoids a cvt dependency and supports even very small terminal sizes.
            const auto width = size.width, height = size.height;
            const double clock = static_cast<double>(width + 48) * (height + 3) * 60 / 1000000;
            run({control_path_, "--newmode", name, std::to_string(clock),
                 std::to_string(width), std::to_string(width + 16), std::to_string(width + 32), std::to_string(width + 48),
                 std::to_string(height), std::to_string(height + 1), std::to_string(height + 2), std::to_string(height + 3)});
            run({control_path_, "--addmode", "VNC-0", name});
            run({control_path_, "--output", "VNC-0", "--mode", name});
            if (!mode_.empty()) {
                run({control_path_, "--delmode", "VNC-0", mode_});
                run({control_path_, "--rmmode", mode_});
            }
            mode_ = name;
        }
        if (owner_->stopping()) return;
        expected_ = size; resize_deadline_ = Clock::now() + std::chrono::seconds(5);
    }
private:
    void validate_size(Size size) const {
        validate_desktop(size);
        if (protocol_ == "x11" && (size.width < 32 || size.height < 32))
            throw std::runtime_error("TigerVNC requires a local X11 desktop of at least 32x32 pixels");
    }
    template<class Ready> bool wait(Ready ready) {
        const auto deadline = Clock::now() + std::chrono::seconds(10);
        while (!owner_->stopping()) {
            check_servers();
            if (ready()) return true;
            if (Clock::now() > deadline) throw server_->failure("startup timed out");
            owner_->poll(20);
        }
        return false;
    }
    void check_servers() {
        if (server_ && !server_->running()) throw server_->failure();
        if (bridge_ && !bridge_->running()) throw bridge_->failure();
    }
    void run(const std::vector<std::string>& command) {
        Child child(command, env_, directory_.file("control.log"));
        const auto deadline = Clock::now() + std::chrono::seconds(5);
        // Do not recursively dispatch Session events while applying a resize.
        while (child.running() && !owner_->stopping()) {
            if (Clock::now() > deadline) throw child.failure("timed out");
            usleep(10000);
        }
        if (!owner_->stopping() && !child.success()) throw child.failure();
    }
    void start_x11(Size size) {
        const auto authority = directory_.file("Xauthority");
        // FamilyWild, empty address/display fields, and a random MIT cookie.
        // Xvnc uses the cookie immediately, before -displayfd chooses a number.
        Descriptor random(open("/dev/urandom", O_RDONLY | O_CLOEXEC));
        std::string cookie(16, '\0');
        if (random.get() < 0 || read(random.get(), cookie.data(), cookie.size()) != static_cast<ssize_t>(cookie.size()))
            throw std::runtime_error("cannot create X11 authentication cookie");
        std::string auth("\xff\xff\0\0\0\0\0\x12", 8);
        auth += "MIT-MAGIC-COOKIE-1"; auth.append("\0\x10", 2); auth += cookie;
        file(authority, auth); env_["XAUTHORITY"] = authority;
        int pipe_fds[2];
        if (pipe(pipe_fds) != 0) throw std::runtime_error("cannot create X11 display pipe");
        Descriptor reader(pipe_fds[0]), writer(pipe_fds[1]);
        if (fcntl(reader.get(), F_SETFD, FD_CLOEXEC) < 0 || fcntl(writer.get(), F_SETFD, FD_CLOEXEC) < 0 ||
            fcntl(reader.get(), F_SETFL, O_NONBLOCK) < 0) throw std::runtime_error("cannot configure X11 display pipe");
        server_ = std::make_unique<Child>(std::vector<std::string>{server_path_, "-displayfd", "3", "-geometry", dimensions(size),
            "-depth", "24", "-auth", authority, "-nolisten", "tcp", "-rfbport", "-1", "-rfbunixpath", directory_.file("rfb"),
            "-rfbunixmode", "0600", "-SecurityTypes", "None", "-AlwaysShared", "-AcceptSetDesktopSize=0", "-noreset"},
            env_, directory_.file("server.log"), writer.get());
        close(writer.release());
        std::string display;
        if (!wait([&] {
            char data[32]; const auto count = read(reader.get(), data, sizeof data);
            if (count > 0) display.append(data, static_cast<size_t>(count));
            else if (count == 0) throw server_->failure("closed the display-number pipe");
            else if (errno != EAGAIN && errno != EINTR) throw std::runtime_error("cannot read X11 display number");
            if (display.size() > 16) throw std::runtime_error("invalid X11 display number");
            return display.find('\n') != std::string::npos;
        })) return;
        display.resize(display.find('\n'));
        if (display.empty() || display.find_first_not_of("0123456789") != std::string::npos)
            throw std::runtime_error("invalid X11 display number");
        env_["DISPLAY"] = ":" + display;
    }
    void start_wayland(Size size) {
        const auto config = directory_.file("sway.conf");
        // Deferred Sway exec commands run after output initialization. Capture
        // its actual environment then, rather than guessing wayland-0/wayland-1
        // or treating socket creation as proof that the compositor is ready.
        file(directory_.file("ready.sh"), "printf '%s\\n%s\\n' \"$WAYLAND_DISPLAY\" \"$SWAYSOCK\" > '" +
             directory_.file("ready.tmp") + "'\nmv '" + directory_.file("ready.tmp") + "' '" + directory_.file("ready") + "'\n");
        file(config, "xwayland disable\nswaybg_command -\nswaynag_command -\ndefault_border none\n"
             "seat seat0 fallback true\noutput HEADLESS-1 mode --custom " + dimensions(size) + " scale 1\n"
             "exec /bin/sh '" + directory_.file("ready.sh") + "'\n");
        Environment server_env = env_;
        server_env["XDG_RUNTIME_DIR"] = directory_.path();
        server_env["WLR_BACKENDS"] = "headless"; server_env["WLR_HEADLESS_OUTPUTS"] = "1";
        server_env["WLR_LIBINPUT_NO_DEVICES"] = "1"; server_env["WLR_RENDERER"] = "pixman";
        server_ = std::make_unique<Child>(std::vector<std::string>{server_path_, "--config", config}, server_env, directory_.file("server.log"));
        env_["XDG_RUNTIME_DIR"] = directory_.path(); env_["XDG_CURRENT_DESKTOP"] = "sway";
        if (!wait([&] {
            std::ifstream ready(directory_.file("ready"));
            if (!ready) return false;
            std::string display, ipc;
            std::getline(ready, display); std::getline(ready, ipc);
            if (display.empty() || display.find('/') != std::string::npos ||
                ipc.rfind(directory_.path() + "/", 0) != 0)
                throw std::runtime_error("invalid local Wayland startup environment");
            env_["WAYLAND_DISPLAY"] = display; env_["SWAYSOCK"] = ipc;
            return true;
        })) return;
        file(directory_.file("wayvnc.conf"), "enable_auth=false\n");
        bridge_ = std::make_unique<Child>(std::vector<std::string>{bridge_path_, "--config", directory_.file("wayvnc.conf"),
            "--unix-socket", "--render-cursor", "--output", "HEADLESS-1", directory_.file("rfb")}, env_, directory_.file("wayvnc.log"));
    }
    std::string protocol_, server_path_, bridge_path_, control_path_, mode_;
    Directory directory_;
    Environment env_;
    Session* owner_ = nullptr;
    std::unique_ptr<Child> server_, bridge_, application_;
    std::unique_ptr<Backend> vnc_;
    Size expected_;
    Clock::time_point resize_deadline_;
};
}
std::unique_ptr<Backend> make_local(const std::string& protocol) {
#ifndef GOBLIN_WITH_VNC
    (void)protocol;
    throw std::runtime_error("local display modes require LibVNCClient support");
#else
    return std::make_unique<Local>(protocol);
#endif
}
}
