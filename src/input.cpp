// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Adam DePrince
#include "input.hpp"
#include <charconv>
#include <vector>

namespace goblin {
namespace {
std::vector<int> numbers(std::string_view text) {
    std::vector<int> result;
    while (!text.empty()) {
        const auto end = text.find(';');
        const auto part = text.substr(0, end);
        int value = 0;
        const auto parsed = std::from_chars(part.data(), part.data() + part.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != part.data() + part.size() || value < 0) return {};
        result.push_back(value);
        if (end == std::string_view::npos) break;
        text.remove_prefix(end + 1);
    }
    return result;
}
uint32_t kitty_key(int code) {
    switch (code) {
    case 27: return uint32_t(Key::escape);
    case 13: return uint32_t(Key::enter);
    case 9: return uint32_t(Key::tab);
    case 127: return uint32_t(Key::backspace);
    case 57348: return uint32_t(Key::insert);
    case 57349: return uint32_t(Key::del);
    case 57350: return uint32_t(Key::left);
    case 57351: return uint32_t(Key::right);
    case 57352: return uint32_t(Key::up);
    case 57353: return uint32_t(Key::down);
    case 57354: return uint32_t(Key::page_up);
    case 57355: return uint32_t(Key::page_down);
    case 57356: return uint32_t(Key::home);
    case 57357: return uint32_t(Key::end);
    case 57441: case 57447: return uint32_t(Key::shift);
    case 57442: case 57448: return uint32_t(Key::control);
    case 57443: case 57449: return uint32_t(Key::alt);
    case 57444: case 57450: return uint32_t(Key::super);
    default:
        if (code >= 57364 && code <= 57375) return uint32_t(Key::f1) + code - 57364;
        // Unmapped functional keys must not become private-use text.
        if (code >= 57344 && code <= 63743) return 0;
        return static_cast<uint32_t>(code);
    }
}
}
void Input::key(uint32_t value, unsigned modifiers, int action, uint32_t physical) {
    if (!value) return;
    Event event; event.key = value; event.modifiers = modifiers; event.action = action;
    event.physical = physical ? physical : value;
    if ((value == ']' && (modifiers & 4)) || value == 29) {
        if (action != 3) { event.type = EventType::quit; receive_(event); }
        return;
    }
    receive_(event);
}
void Input::csi(std::string_view seq) {
    if (seq.empty()) return;
    const char final = seq.back(); seq.remove_suffix(1);
    if (final == 't') {
        const auto values = numbers(seq);
        Event event;
        if (values.size() == 3 && values[0] == 6) {
            event.type = EventType::cell_size; event.size = {values[2], values[1]};
        } else if (values.size() == 3 && values[0] == 8) {
            event.type = EventType::terminal_size; event.size = {values[2], values[1]};
        } else if (values.size() == 3 && values[0] == 4) {
            event.type = EventType::pixel_size; event.size = {values[2], values[1]};
        } else if (values.size() == 5 && values[0] == 48) {
            // Minimized windows can report zero cells. Retain the last usable
            // geometry until a nonzero report arrives.
            if (values[1] == 0 || values[2] == 0) return;
            event.type = EventType::terminal_size; event.size = {values[2], values[1]};
            receive_(event);
            if (values[1] > 0 && values[2] > 0 && values[3] > 0 && values[4] > 0) {
                event.type = EventType::cell_size;
                event.size = {values[4] / values[2], values[3] / values[1]};
                receive_(event);
                event.type = EventType::pixel_size;
                event.size = {values[4], values[3]};
                receive_(event);
            }
            return;
        } else return;
        if (event.size.width > 0 && event.size.height > 0) receive_(event);
        return;
    }
    if (final == 'y' && seq.substr(0, 6) == "?1016;" && seq.back() == '$') {
        seq.remove_prefix(6); seq.remove_suffix(1);
        const auto values = numbers(seq);
        if (values.size() == 1) {
            Event event; event.type = EventType::mouse_mode;
            event.action = values[0]; receive_(event);
        }
        return;
    }
    if ((final == 'M' || final == 'm') && !seq.empty() && seq.front() == '<') {
        seq.remove_prefix(1); const auto values = numbers(seq);
        if (values.size() != 3 || values[1] < 1 || values[2] < 1) return;
        Event event; event.type = EventType::mouse; event.button = values[0];
        event.modifiers = ((values[0] & 4) ? 1u : 0u) | ((values[0] & 8) ? 2u : 0u) |
                          ((values[0] & 16) ? 4u : 0u);
        event.x = values[1] - 1; event.y = values[2] - 1;
        event.action = final == 'm' ? 3 : 1;
        receive_(event); return;
    }
    if (final == 'u') {
        // Kitty permits alternate key codes and modifier:event subparameters.
        const auto semicolon = seq.find(';');
        auto all_codes = seq.substr(0, semicolon);
        const auto code_text = all_codes.substr(0, all_codes.find(':'));
        const auto codes = numbers(code_text);
        if (codes.size() != 1) return;
        unsigned mods = 0; int action = 1;
        if (semicolon != std::string_view::npos) {
            auto mod_text = seq.substr(semicolon + 1);
            mod_text = mod_text.substr(0, mod_text.find(';'));
            const auto colon = mod_text.find(':');
            const auto values = numbers(mod_text.substr(0, colon));
            if (values.size() != 1 || values[0] < 1) return;
            mods = static_cast<unsigned>(values[0] - 1) & 15;
            if (colon != std::string_view::npos) {
                const auto actions = numbers(mod_text.substr(colon + 1));
                if (actions.size() != 1 || actions[0] < 1 || actions[0] > 3) return;
                action = actions[0];
            }
        }
        int code = codes[0];
        if ((mods & 1) && !(mods & 14)) {
            const auto colon = all_codes.find(':');
            if (colon != std::string_view::npos) {
                auto shifted = all_codes.substr(colon + 1);
                shifted = shifted.substr(0, shifted.find(':'));
                const auto alternate = numbers(shifted);
                if (alternate.size() == 1 && alternate[0] > 0) code = alternate[0];
            } else if (code >= 'a' && code <= 'z') code += 'A' - 'a';
        }
        if (code <= 0x10ffff && !(code >= 0xd800 && code <= 0xdfff))
            key(kitty_key(code), mods, action, kitty_key(codes[0]));
        return;
    }
    int action = 0;
    const auto colon = seq.find(':');
    if (colon != std::string_view::npos) {
        const auto actions = numbers(seq.substr(colon + 1));
        if (actions.size() != 1 || actions[0] < 1 || actions[0] > 3) return;
        action = actions[0]; seq = seq.substr(0, colon);
    }
    const auto values = seq.empty() ? std::vector<int>{} : numbers(seq);
    const unsigned mods = values.size() > 1 && values[1] > 0 ? unsigned(values[1] - 1) & 15 : 0;
    switch (final) {
    case 'A': key(uint32_t(Key::up), mods, action); break;
    case 'B': key(uint32_t(Key::down), mods, action); break;
    case 'C': key(uint32_t(Key::right), mods, action); break;
    case 'D': key(uint32_t(Key::left), mods, action); break;
    case 'H': key(uint32_t(Key::home), mods, action); break;
    case 'F': key(uint32_t(Key::end), mods, action); break;
    case 'P': key(uint32_t(Key::f1), mods, action); break;
    case 'Q': key(uint32_t(Key::f2), mods, action); break;
    case 'S': key(uint32_t(Key::f4), mods, action); break;
    case 'Z': key(uint32_t(Key::tab), mods | 1, action); break;
    case '~':
        if (values.empty()) break;
        switch (values[0]) {
        case 1: case 7: key(uint32_t(Key::home), mods, action); break;
        case 2: key(uint32_t(Key::insert), mods, action); break;
        case 3: key(uint32_t(Key::del), mods, action); break;
        case 4: case 8: key(uint32_t(Key::end), mods, action); break;
        case 5: key(uint32_t(Key::page_up), mods, action); break;
        case 6: key(uint32_t(Key::page_down), mods, action); break;
        default: {
            static const int function_codes[] = {11,12,13,14,15,17,18,19,20,21,23,24};
            for (int i = 0; i < 12; ++i) if (values[0] == function_codes[i]) key(uint32_t(Key::f1) + i, mods, action);
        }}
        break;
    }
}
void Input::feed(std::string_view bytes) {
    pending_.append(bytes);
    while (!pending_.empty()) {
        const auto byte = static_cast<uint8_t>(pending_[0]);
        if (byte == 27) {
            if (pending_.size() == 1) return;
            const char next = pending_[1];
            if (next == '[') {
                size_t end = 2;
                while (end < pending_.size() && !(pending_[end] >= '@' && pending_[end] <= '~')) ++end;
                if (end == pending_.size()) return;
                csi(std::string_view(pending_).substr(2, end - 1));
                pending_.erase(0, end + 1); continue;
            }
            // Consume terminal replies; they must never become remote keystrokes.
            if (next == '_' || next == 'P' || next == ']') {
                const auto end = pending_.find("\033\\", 2);
                const auto bell = next == ']' ? pending_.find('\a', 2) : std::string::npos;
                if (end == std::string::npos && bell == std::string::npos) {
                    return;
                }
                if (next == '_' && end != std::string::npos &&
                    std::string_view(pending_).substr(2, end - 2) ==
                        "Gi=" + std::to_string(hdr_query_id) + ";OK") {
                    Event event; event.type = EventType::hdr_format; receive_(event);
                }
                if (bell < end) pending_.erase(0, bell + 1);
                else pending_.erase(0, end + 2);
                continue;
            }
            if (next == 'O') {
                if (pending_.size() < 3) return;
                const char final = pending_[2];
                if (final >= 'P' && final <= 'S') key(uint32_t(Key::f1) + final - 'P');
                else csi(std::string_view(pending_).substr(2, 1));
                pending_.erase(0, 3); continue;
            }
        }
        const size_t start = byte == 27 ? 1 : 0;
        const auto lead = static_cast<uint8_t>(pending_[start]);
        size_t length = lead < 128 ? 1 : lead >= 0xc2 && lead < 0xe0 ? 2 : lead < 0xf0 && lead >= 0xe0 ? 3 : lead >= 0xf0 && lead <= 0xf4 ? 4 : 1;
        if (pending_.size() < start + length) return;
        uint32_t code = lead & (length == 1 ? 0xff : (1 << (7 - length)) - 1);
        bool valid = length != 1 || lead < 128;
        for (size_t i = 1; i < length; ++i) {
            const auto continuation = static_cast<uint8_t>(pending_[start + i]);
            valid &= (continuation & 0xc0) == 0x80;
            code = (code << 6) | (continuation & 63);
        }
        valid &= code <= 0x10ffff && !(code >= 0xd800 && code <= 0xdfff);
        valid &= length == 1 || code >= (length == 2 ? 0x80u : length == 3 ? 0x800u : 0x10000u);
        if (valid) {
            unsigned mods = start ? 2 : 0;
            if (code == 29 && !start) key(29);
            else if (code == 13 || code == 10) key(uint32_t(Key::enter), mods);
            else if (code == 9) key(uint32_t(Key::tab), mods);
            else if (code == 127 || code == 8) key(uint32_t(Key::backspace), mods);
            else if (code > 0 && code < 27) key('a' + code - 1, mods | 4);
            else if (code == 0) key(' ', mods | 4);
            else key(code, mods);
        }
        pending_.erase(0, start + length);
    }
}
void Input::flush_escape() {
    if (pending_ == "\033") { pending_.clear(); key(uint32_t(Key::escape)); }
}
}
