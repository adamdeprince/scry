// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Adam DePrince
#include "session.hpp"
#include <stdexcept>
#ifdef GOBLIN_WITH_RDP
#include <freerdp/freerdp.h>
#include <freerdp/scancode.h>
#include <winpr/synch.h>
#include <winpr/wlog.h>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace goblin {
namespace {
struct Context { rdpContext base; Session* session; };
struct Pointer { rdpPointer base; uint8_t* rgba; };
Session& session(rdpContext* context) { return *reinterpret_cast<Context*>(context)->session; }
template<class Function> BOOL callback(rdpContext* context, Function function) noexcept {
    try { function(); return TRUE; }
    catch (...) { session(context).fail(std::current_exception()); return FALSE; }
}
BOOL begin_paint(rdpContext* context) {
    auto* window = context->gdi->primary->hdc->hwnd;
    window->invalid->null = TRUE; window->ninvalid = 0; return TRUE;
}
BOOL end_paint(rdpContext* context) {
    return callback(context, [&] {
        auto* window = context->gdi->primary->hdc->hwnd;
        if (window->invalid->null) return;
        if (window->ninvalid > 0) {
            for (int i = 0; i < window->ninvalid; ++i) {
                const auto& rect = window->cinvalid[i];
                session(context).damage(rect.x, rect.y, rect.w, rect.h);
            }
        } else {
            const auto* rect = window->invalid;
            session(context).damage(rect->x, rect->y, rect->w, rect->h);
        }
    });
}
BOOL desktop_resize(rdpContext* context) {
    return callback(context, [&] {
        const Size size{static_cast<int>(freerdp_settings_get_uint32(context->settings, FreeRDP_DesktopWidth)),
                        static_cast<int>(freerdp_settings_get_uint32(context->settings, FreeRDP_DesktopHeight))};
        validate_desktop(size);
        if (!gdi_resize(context->gdi, size.width, size.height)) throw std::runtime_error("RDP framebuffer resize failed");
        session(context).resize(size);
    });
}
BOOL pointer_new(rdpContext* context, rdpPointer* pointer) {
    return callback(context, [&] {
        if (pointer->width > 256 || pointer->height > 256) throw std::runtime_error("RDP cursor exceeds 256 pixels");
        auto* custom = reinterpret_cast<Pointer*>(pointer);
        custom->rgba = static_cast<uint8_t*>(calloc(static_cast<size_t>(pointer->width) * pointer->height, 4));
        if (!custom->rgba || !freerdp_image_copy_from_pointer_data(custom->rgba, PIXEL_FORMAT_RGBA32,
            pointer->width * 4, 0, 0, pointer->width, pointer->height, pointer->xorMaskData,
            pointer->lengthXorMask, pointer->andMaskData, pointer->lengthAndMask, pointer->xorBpp, &context->gdi->palette))
            throw std::runtime_error("RDP cursor decode failed");
    });
}
void pointer_free(rdpContext*, rdpPointer* pointer) { free(reinterpret_cast<Pointer*>(pointer)->rgba); }
BOOL pointer_set(rdpContext* context, rdpPointer* pointer) {
    return callback(context, [&] {
        const auto* data = reinterpret_cast<Pointer*>(pointer)->rgba;
        session(context).renderer().cursor_shape({static_cast<int>(pointer->width), static_cast<int>(pointer->height)},
            {static_cast<int>(pointer->xPos), static_cast<int>(pointer->yPos)},
            std::string(reinterpret_cast<const char*>(data), static_cast<size_t>(pointer->width) * pointer->height * 4));
    });
}
BOOL pointer_null(rdpContext* context) {
    return callback(context, [&] { session(context).renderer().cursor_shape({}, {}, {}); });
}
BOOL pointer_default(rdpContext* context) {
    return callback(context, [&] {
        std::string rgba(12 * 18 * 4, '\0');
        for (int y = 0; y < 18; ++y) for (int x = 0; x < 12; ++x) {
            if (x <= y / 2 && (y < 12 || (x >= 3 && x <= 5))) {
                const size_t offset = (y * 12 + x) * 4;
                rgba[offset] = rgba[offset + 1] = rgba[offset + 2] = x == 0 || x == y / 2 ? 0 : char(255);
                rgba[offset + 3] = char(255);
            }
        }
        session(context).renderer().cursor_shape({12, 18}, {}, std::move(rgba));
    });
}
BOOL pointer_position(rdpContext* context, UINT32 x, UINT32 y) {
    return callback(context, [&] { session(context).renderer().cursor_position(x, y); });
}
BOOL post_connect(freerdp* instance) {
    auto* settings = instance->context->settings;
    if (!callback(instance->context, [&] {
        validate_desktop({static_cast<int>(freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth)),
                          static_cast<int>(freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight))});
    })) return FALSE;
    if (!gdi_init(instance, PIXEL_FORMAT_RGBX32)) return FALSE;
    auto* context = instance->context;
    context->update->BeginPaint = begin_paint; context->update->EndPaint = end_paint;
    context->update->DesktopResize = desktop_resize;
    rdpPointer pointer{};
    pointer.size = sizeof(Pointer); pointer.New = pointer_new; pointer.Free = pointer_free;
    pointer.Set = pointer_set; pointer.SetNull = pointer_null; pointer.SetDefault = pointer_default;
    pointer.SetPosition = pointer_position;
    graphics_register_pointer(context->graphics, &pointer);
    return callback(context, [&] { session(context).resize({context->gdi->width, context->gdi->height}); });
}
DWORD reject_certificate(freerdp*, const char*, UINT16, const char*, const char*, const char*, const char*, DWORD) { return 0; }
DWORD reject_changed(freerdp*, const char*, UINT16, const char*, const char*, const char*, const char*,
                     const char*, const char*, const char*, DWORD) { return 0; }
UINT32 scancode(uint32_t key) {
    switch (static_cast<Key>(key)) {
    case Key::escape: return RDP_SCANCODE_ESCAPE;
    case Key::enter: return RDP_SCANCODE_RETURN;
    case Key::tab: return RDP_SCANCODE_TAB;
    case Key::backspace: return RDP_SCANCODE_BACKSPACE;
    case Key::up: return RDP_SCANCODE_UP;
    case Key::down: return RDP_SCANCODE_DOWN;
    case Key::left: return RDP_SCANCODE_LEFT;
    case Key::right: return RDP_SCANCODE_RIGHT;
    case Key::home: return RDP_SCANCODE_HOME;
    case Key::end: return RDP_SCANCODE_END;
    case Key::page_up: return RDP_SCANCODE_PRIOR;
    case Key::page_down: return RDP_SCANCODE_NEXT;
    case Key::insert: return RDP_SCANCODE_INSERT;
    case Key::del: return RDP_SCANCODE_DELETE;
    case Key::shift: return RDP_SCANCODE_LSHIFT;
    case Key::control: return RDP_SCANCODE_LCONTROL;
    case Key::alt: return RDP_SCANCODE_LMENU;
    case Key::super: return RDP_SCANCODE_LWIN;
    default: break;
    }
    static const UINT32 functions[] = {RDP_SCANCODE_F1,RDP_SCANCODE_F2,RDP_SCANCODE_F3,RDP_SCANCODE_F4,
        RDP_SCANCODE_F5,RDP_SCANCODE_F6,RDP_SCANCODE_F7,RDP_SCANCODE_F8,RDP_SCANCODE_F9,RDP_SCANCODE_F10,
        RDP_SCANCODE_F11,RDP_SCANCODE_F12};
    if (key >= uint32_t(Key::f1) && key <= uint32_t(Key::f12)) return functions[key - uint32_t(Key::f1)];
    static const UINT32 letters[] = {0x1e,0x30,0x2e,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,
        0x31,0x18,0x19,0x10,0x13,0x1f,0x14,0x16,0x2f,0x11,0x2d,0x15,0x2c};
    if (key >= 'A' && key <= 'Z') key += 'a' - 'A';
    if (key >= 'a' && key <= 'z') return letters[key - 'a'];
    if (key >= '1' && key <= '9') return key - '1' + 2;
    switch (key) {
    case '0': return 0x0b; case ' ': return 0x39; case '-': return 0x0c; case '=': return 0x0d;
    case '[': return 0x1a; case ']': return 0x1b; case '\\': return 0x2b; case ';': return 0x27;
    case '\'': return 0x28; case '`': return 0x29; case ',': return 0x33; case '.': return 0x34; case '/': return 0x35;
    default: return 0;
    }
}
class Rdp final : public Backend {
public:
    ~Rdp() override {
        if (instance_) {
            if (instance_->context) {
                if (connected_) {
                    auto* input = instance_->context->input;
                    for (const auto& entry : held_keys_) {
                        const auto state = entry.second;
                        if (state.second) freerdp_input_send_keyboard_event_ex(input, FALSE, FALSE, state.first);
                        else if (state.first <= 0xffff)
                            freerdp_input_send_unicode_keyboard_event(input, KBD_FLAGS_RELEASE, state.first);
                        else {
                            freerdp_input_send_unicode_keyboard_event(input, KBD_FLAGS_RELEASE, 0xd800 + ((state.first - 0x10000) >> 10));
                            freerdp_input_send_unicode_keyboard_event(input, KBD_FLAGS_RELEASE, 0xdc00 + ((state.first - 0x10000) & 1023));
                        }
                    }
                    static constexpr UINT32 codes[] = {RDP_SCANCODE_LSHIFT,RDP_SCANCODE_LMENU,RDP_SCANCODE_LCONTROL,RDP_SCANCODE_LWIN};
                    for (int i = 0; i < 4; ++i) if (modifiers_ & (1u << i))
                        freerdp_input_send_keyboard_event_ex(input, FALSE, FALSE, codes[i]);
                    static constexpr UINT16 buttons[] = {PTR_FLAGS_BUTTON1,PTR_FLAGS_BUTTON3,PTR_FLAGS_BUTTON2};
                    for (int i = 0; i < 3; ++i) if (mouse_mask_ & (1 << i))
                        freerdp_input_send_mouse_event(input, buttons[i], mouse_x_, mouse_y_);
                }
                freerdp_disconnect(instance_);
                if (instance_->context->gdi) gdi_free(instance_);
                freerdp_context_free(instance_);
            }
            freerdp_free(instance_);
        }
    }
    void connect(Session& owner, const Options& options) override {
        WLog_SetLogLevel(WLog_GetRoot(), WLOG_OFF);
        instance_ = freerdp_new();
        if (!instance_) throw std::runtime_error("cannot allocate FreeRDP instance");
        instance_->ContextSize = sizeof(Context);
        if (!freerdp_context_new(instance_)) throw std::runtime_error("cannot allocate FreeRDP context");
        reinterpret_cast<Context*>(instance_->context)->session = &owner;
        owner_ = &owner; instance_->PostConnect = post_connect;
        instance_->VerifyCertificateEx = reject_certificate; instance_->VerifyChangedCertificateEx = reject_changed;
        auto* settings = instance_->context->settings;
        bool ok = freerdp_settings_set_string(settings, FreeRDP_ServerHostname, options.host.c_str()) &&
            freerdp_settings_set_uint32(settings, FreeRDP_ServerPort, options.port ? options.port : 3389) &&
            freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth, options.desktop.width) &&
            freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight, options.desktop.height) &&
            freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, 32) &&
            freerdp_settings_set_string(settings, FreeRDP_Username, options.username.c_str()) &&
            freerdp_settings_set_string(settings, FreeRDP_Password, options.password.c_str()) &&
            freerdp_settings_set_string(settings, FreeRDP_Domain, options.domain.c_str()) &&
            freerdp_settings_set_bool(settings, FreeRDP_SoftwareGdi, TRUE) &&
            freerdp_settings_set_bool(settings, FreeRDP_SupportGraphicsPipeline, FALSE) &&
            freerdp_settings_set_bool(settings, FreeRDP_IgnoreCertificate, options.insecure);
        if (!ok) throw std::runtime_error("cannot configure FreeRDP settings");
        if (!freerdp_connect(instance_)) {
            owner.check_error();
            throw std::runtime_error("RDP connection failed: " + std::string(freerdp_get_last_error_name(freerdp_get_last_error(instance_->context))));
        }
        owner.check_error();
        connected_ = true;
    }
    bool step(int timeout_ms) override {
        if (freerdp_shall_disconnect_context(instance_->context)) {
            const auto error = freerdp_get_last_error(instance_->context);
            if (error) throw std::runtime_error("RDP disconnected: " + std::string(freerdp_get_last_error_name(error)));
            return false;
        }
        HANDLE handles[MAXIMUM_WAIT_OBJECTS];
        const auto count = freerdp_get_event_handles(instance_->context, handles, MAXIMUM_WAIT_OBJECTS);
        if (!count || WaitForMultipleObjects(count, handles, FALSE, timeout_ms) == WAIT_FAILED)
            throw std::runtime_error("RDP event wait failed");
        if (!freerdp_check_event_handles(instance_->context)) {
            owner_->check_error();
            const auto error = freerdp_get_last_error(instance_->context);
            if (error) throw std::runtime_error("RDP session failed: " + std::string(freerdp_get_last_error_name(error)));
            return false;
        }
        return true;
    }
    Frame frame() const override {
        const auto* gdi = instance_->context->gdi;
        return {gdi->primary_buffer, {gdi->width, gdi->height}, gdi->stride, 0, 1, 2};
    }
    void key(const Event& event) override {
        auto* input = instance_->context->input;
        struct State { uint32_t value; bool scan; };
        const auto code = scancode(event.key);
        State state{event.key, false};
        if (code && (event.key >= uint32_t(Key::escape) || (event.modifiers & (2 | 4 | 8)))) state = {code, true};
        const auto held = held_keys_.find(event.physical);
        if (held != held_keys_.end()) state = {held->second.first, held->second.second};
        const bool modifier = event.key >= uint32_t(Key::shift) && event.key <= uint32_t(Key::super);
        if (!modifier) modifiers(event.modifiers);
        const bool down = event.action != 3;
        if (state.scan) {
            send_scan(state.value, down, event.action == 2);
            if (event.action == 0) send_scan(state.value, false, false);
        } else if (state.value <= 0x10ffff) {
            const auto unicode = [&](UINT16 value) {
                if (!freerdp_input_send_unicode_keyboard_event(input, down ? 0 : KBD_FLAGS_RELEASE, value))
                    throw std::runtime_error("RDP key send failed");
                if (event.action == 0 && !freerdp_input_send_unicode_keyboard_event(input, KBD_FLAGS_RELEASE, value))
                    throw std::runtime_error("RDP key release failed");
            };
            if (state.value <= 0xffff) unicode(state.value);
            else { unicode(0xd800 + ((state.value - 0x10000) >> 10)); unicode(0xdc00 + ((state.value - 0x10000) & 1023)); }
        }
        if (event.action == 3) held_keys_.erase(event.physical);
        else if (event.action != 0) held_keys_[event.physical] = {state.value, state.scan};
        if (modifier) {
            const unsigned bit = 1u << (event.key - uint32_t(Key::shift));
            // enum order differs from the protocol's modifier bit order.
            const unsigned actual = bit == 2 ? 4 : bit == 4 ? 2 : bit;
            if (down) modifiers_ |= actual; else modifiers_ &= ~actual;
        }
        if (event.action == 0) modifiers(0);
    }
    void pointer(int x, int y, int mask, int wheel, unsigned desired_modifiers) override {
        modifiers(desired_modifiers | (modifiers_ & 8));
        mouse_x_ = x; mouse_y_ = y;
        auto* input = instance_->context->input;
        const auto send = [&](UINT16 flags) {
            if (!freerdp_input_send_mouse_event(input, flags, x, y)) throw std::runtime_error("RDP mouse send failed");
        };
        send(PTR_FLAGS_MOVE);
        static constexpr UINT16 buttons[] = {PTR_FLAGS_BUTTON1, PTR_FLAGS_BUTTON3, PTR_FLAGS_BUTTON2};
        for (int i = 0; i < 3; ++i) if ((mask ^ mouse_mask_) & (1 << i))
            send(buttons[i] | ((mask & (1 << i)) ? PTR_FLAGS_DOWN : 0));
        mouse_mask_ = mask;
        if (wheel) send((wheel == 2 || wheel == -2 ? PTR_FLAGS_HWHEEL : PTR_FLAGS_WHEEL) |
                        (wheel < 0 ? PTR_FLAGS_WHEEL_NEGATIVE | 0x188 : 120));
        owner_->renderer().cursor_position(x, y);
    }
private:
    void send_scan(UINT32 code, bool down, bool repeat) {
        if (!freerdp_input_send_keyboard_event_ex(instance_->context->input, down, repeat, code))
            throw std::runtime_error("RDP keyboard send failed");
    }
    void modifiers(unsigned desired) {
        static constexpr UINT32 codes[] = {RDP_SCANCODE_LSHIFT, RDP_SCANCODE_LMENU, RDP_SCANCODE_LCONTROL, RDP_SCANCODE_LWIN};
        for (int i = 0; i < 4; ++i) if ((desired ^ modifiers_) & (1u << i)) send_scan(codes[i], desired & (1u << i), false);
        modifiers_ = desired;
    }
    freerdp* instance_ = nullptr;
    Session* owner_ = nullptr;
    unsigned modifiers_ = 0;
    int mouse_mask_ = 0;
    std::unordered_map<uint32_t, std::pair<uint32_t, bool>> held_keys_;
    bool connected_ = false;
    int mouse_x_ = 0, mouse_y_ = 0;
};
}
std::unique_ptr<Backend> make_rdp() { return std::make_unique<Rdp>(); }
}
#else
namespace goblin { std::unique_ptr<Backend> make_rdp() { throw std::runtime_error("built without FreeRDP support"); } }
#endif
