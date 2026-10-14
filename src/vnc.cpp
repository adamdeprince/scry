// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Adam DePrince
#include "session.hpp"
#include <stdexcept>
#ifdef GOBLIN_WITH_VNC
#include <rfb/rfbclient.h>
#include <arpa/inet.h>
#include <cstdarg>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <unordered_map>

namespace goblin {
namespace {
int client_tag;
std::string last_message;
void log_message(const char* format, ...) {
    char message[2048]; va_list args; va_start(args, format);
    vsnprintf(message, sizeof message, format, args); va_end(args);
    try { last_message = message; } catch (...) {}
}
uint32_t keysym(uint32_t key) {
    switch (static_cast<Key>(key)) {
    case Key::escape: return 0xff1b; case Key::enter: return 0xff0d;
    case Key::tab: return 0xff09; case Key::backspace: return 0xff08;
    case Key::up: return 0xff52; case Key::down: return 0xff54;
    case Key::left: return 0xff51; case Key::right: return 0xff53;
    case Key::home: return 0xff50; case Key::end: return 0xff57;
    case Key::page_up: return 0xff55; case Key::page_down: return 0xff56;
    case Key::insert: return 0xff63; case Key::del: return 0xffff;
    case Key::shift: return 0xffe1; case Key::control: return 0xffe3;
    case Key::alt: return 0xffe9; case Key::super: return 0xffeb;
    default:
        if (key >= uint32_t(Key::f1) && key <= uint32_t(Key::f12)) return 0xffbe + key - uint32_t(Key::f1);
        return key <= 0xff ? key : 0x01000000 | key;
    }
}
class Vnc final : public Backend {
public:
    explicit Vnc(int connected_socket) : connected_socket_(connected_socket) {}
    ~Vnc() override {
        if (client_) {
            if (connected_) {
                for (const auto& key : held_keys_) SendKeyEvent(client_, key.second, FALSE);
                static constexpr uint32_t symbols[] = {0xffe1,0xffe9,0xffe3,0xffeb};
                for (int i = 0; i < 4; ++i) if (modifiers_ & (1u << i)) SendKeyEvent(client_, symbols[i], FALSE);
                if (mouse_mask_) SendPointerEvent(client_, mouse_x_, mouse_y_, 0);
            }
            rfbClientCleanup(client_);
        }
        if (connected_socket_ >= 0) close(connected_socket_);
    }
    void connect(Session& owner, const Options& options) override {
        owner_ = &owner; options_ = options; last_message.clear();
        rfbClientLog = log_message; rfbClientErr = log_message;
        client_ = rfbGetClient(8, 3, 4);
        if (!client_) throw std::runtime_error("cannot allocate LibVNCClient");
        rfbClientSetClientData(client_, &client_tag, this);
        free(client_->serverHost); client_->serverHost = strdup(options.host.c_str());
        if (!client_->serverHost) throw std::bad_alloc();
        client_->serverPort = options.port ? options.port : 5900;
        if (connected_socket_ >= 0) {
            client_->sock = connected_socket_; connected_socket_ = -1;
            client_->listenSpecified = TRUE;
        }
        client_->connectTimeout = 10; client_->readTimeout = 10;
        client_->canHandleNewFBSize = TRUE;
        client_->appData.shareDesktop = TRUE;
        client_->appData.viewOnly = options.view_only;
        client_->appData.useRemoteCursor = FALSE; // ask the server to include its cursor
        client_->appData.enableJPEG = FALSE; // lossless cell comparison
        client_->appData.encodingsString = "tight zrle hextile copyrect raw";
        client_->format.bitsPerPixel = 32; client_->format.depth = 24;
        client_->format.bigEndian = 0; client_->format.trueColour = 1;
        client_->format.redMax = client_->format.greenMax = client_->format.blueMax = 255;
        client_->format.redShift = 0; client_->format.greenShift = 8; client_->format.blueShift = 16;
        client_->GetPassword = password;
        if (!options.username.empty()) client_->GetCredential = credential;
        client_->MallocFrameBuffer = allocate;
        client_->GotFrameBufferUpdate = update;
        // rfbInitClient owns and frees the client on initialization failure.
        if (!rfbInitClient(client_, nullptr, nullptr)) {
            client_ = nullptr;
            owner.check_error();
            throw std::runtime_error("VNC connection failed: " + last_message);
        }
        owner.check_error();
        connected_ = true;
        if (options.protocol == "x11" || options.protocol == "wayland") {
            // Local resizing uses RandR / Sway IPC. Request DesktopSize rather
            // than ExtendedDesktopSize: LibVNCClient 0.9.15 ignores screen ID 0,
            // which is valid and used by wayvnc, including when its size changes.
            const uint32_t encodings[] = {rfbEncodingTight, rfbEncodingZRLE, rfbEncodingHextile,
                rfbEncodingCopyRect, rfbEncodingRaw, rfbEncodingCompressLevel1,
                rfbEncodingKeyboardLedState, rfbEncodingNewFBSize, rfbEncodingLastRect,
                rfbEncodingSupportedMessages, rfbEncodingSupportedEncodings, rfbEncodingServerIdentity,
                rfbEncodingXvp, rfbEncodingQemuExtendedKeyEvent};
            // Keep capabilities advertised by rfbInitClient: TigerVNC may have
            // queued their replies and rejects a capability removed meanwhile.
            rfbSetEncodingsMsg message{}; message.type = rfbSetEncodings;
            message.nEncodings = htons(sizeof encodings / sizeof *encodings);
            std::string bytes(reinterpret_cast<const char*>(&message), sz_rfbSetEncodingsMsg);
            for (auto encoding : encodings) {
                encoding = htonl(encoding);
                bytes.append(reinterpret_cast<const char*>(&encoding), sizeof encoding);
            }
            if (!WriteToRFBServer(client_, bytes.data(), static_cast<unsigned>(bytes.size())))
                throw std::runtime_error("cannot configure local VNC resize notifications");
        }
    }
    bool step(int timeout_ms) override {
        // WaitForMessage also checks bytes already buffered inside LibVNCClient.
        const int ready = WaitForMessage(client_, timeout_ms * 1000);
        if (ready < 0 && errno == EINTR) return true;
        if (ready < 0) throw std::runtime_error("VNC wait failed: " + last_message);
        if (ready > 0 && !HandleRFBServerMessage(client_)) {
            owner_->check_error();
            throw std::runtime_error("VNC disconnected: " + last_message);
        }
        owner_->check_error(); return true;
    }
    Frame frame() const override {
        return {have_frame_ ? pixels_.data() : nullptr, {client_->width, client_->height}, static_cast<size_t>(client_->width) * 4, 0, 1, 2};
    }
    void key(const Event& event) override {
        const bool modifier = event.key >= uint32_t(Key::shift) && event.key <= uint32_t(Key::super);
        if (!modifier) modifiers(event.modifiers);
        auto symbol = keysym(event.key);
        const auto held = held_keys_.find(event.physical);
        if (held != held_keys_.end()) symbol = held->second;
        send_key(symbol, event.action != 3);
        if (event.action == 0) send_key(symbol, false);
        else if (event.action == 3) held_keys_.erase(event.physical);
        else held_keys_[event.physical] = symbol;
        if (modifier) {
            unsigned bit = 1u << (event.key - uint32_t(Key::shift));
            if (bit == 2) bit = 4; else if (bit == 4) bit = 2;
            if (event.action == 3) modifiers_ &= ~bit; else modifiers_ |= bit;
        }
        if (event.action == 0) modifiers(0);
    }
    void pointer(int x, int y, int mask, int wheel, unsigned desired_modifiers) override {
        modifiers(desired_modifiers | (modifiers_ & 8));
        mouse_x_ = x; mouse_y_ = y; mouse_mask_ = mask;
        if (!SendPointerEvent(client_, x, y, mask)) throw std::runtime_error("VNC pointer send failed");
        if (wheel) {
            const int wheel_mask = wheel == 1 ? 8 : wheel == -1 ? 16 : wheel == -2 ? 32 : 64;
            if (!SendPointerEvent(client_, x, y, mask | wheel_mask) ||
                !SendPointerEvent(client_, x, y, mask)) throw std::runtime_error("VNC scroll send failed");
        }
    }
private:
    static Vnc& self(rfbClient* client) { return *static_cast<Vnc*>(rfbClientGetClientData(client, &client_tag)); }
    static char* password(rfbClient* client) { return strdup(self(client).options_.password.c_str()); }
    static rfbCredential* credential(rfbClient* client, int type) {
        if (type != rfbCredentialTypeUser) return nullptr;
        const auto& options = self(client).options_;
        auto* result = static_cast<rfbCredential*>(calloc(1, sizeof(rfbCredential)));
        if (!result) return nullptr;
        result->userCredential.username = strdup(options.username.c_str());
        result->userCredential.password = strdup(options.password.c_str());
        if (!result->userCredential.username || !result->userCredential.password) {
            free(result->userCredential.username); free(result->userCredential.password); free(result); return nullptr;
        }
        return result; // LibVNCClient frees the structure and strings
    }
    static rfbBool allocate(rfbClient* client) noexcept {
        auto& backend = self(client);
        try {
            const Size size{client->width, client->height};
            validate_desktop(size);
            backend.have_frame_ = false;
            backend.pixels_.assign(static_cast<size_t>(size.width) * size.height * 4, 0);
            client->frameBuffer = backend.pixels_.data();
            backend.owner_->resize(size);
            return TRUE;
        } catch (...) { backend.owner_->fail(std::current_exception()); return FALSE; }
    }
    static void update(rfbClient* client, int x, int y, int width, int height) noexcept {
        auto& backend = self(client);
        try {
            if (width > 0 && height > 0) backend.have_frame_ = true;
            backend.owner_->damage(x, y, width, height);
        }
        catch (...) { backend.owner_->fail(std::current_exception()); }
    }
    void send_key(uint32_t symbol, bool down) {
        if (!SendKeyEvent(client_, symbol, down)) throw std::runtime_error("VNC keyboard send failed");
    }
    void modifiers(unsigned desired) {
        static constexpr uint32_t symbols[] = {0xffe1, 0xffe9, 0xffe3, 0xffeb};
        for (int i = 0; i < 4; ++i) if ((desired ^ modifiers_) & (1u << i)) send_key(symbols[i], desired & (1u << i));
        modifiers_ = desired;
    }
    rfbClient* client_ = nullptr;
    Session* owner_ = nullptr;
    Options options_;
    std::vector<uint8_t> pixels_;
    unsigned modifiers_ = 0;
    int connected_socket_ = -1;
    std::unordered_map<uint32_t, uint32_t> held_keys_;
    bool connected_ = false, have_frame_ = false;
    int mouse_x_ = 0, mouse_y_ = 0, mouse_mask_ = 0;
};
}
std::unique_ptr<Backend> make_vnc(int connected_socket) { return std::make_unique<Vnc>(connected_socket); }
}
#else
namespace goblin { std::unique_ptr<Backend> make_vnc(int) { throw std::runtime_error("built without LibVNCClient support"); } }
#endif
