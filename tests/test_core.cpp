// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Adam DePrince
#include "input.hpp"
#include <climits>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace goblin;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
size_t count(std::string_view bytes, std::string_view needle) {
    size_t result = 0, offset = 0;
    while ((offset = bytes.find(needle, offset)) != std::string_view::npos) { ++result; offset += needle.size(); }
    return result;
}
void render_tests() {
    require(grid_size({23, 13}, {8, 6}) == Size{3, 3}, "ceil geometry");
    require(base64("f") == "Zg==" && base64("fo") == "Zm8=" && base64("foo") == "Zm9v", "base64 padding");
    std::string output;
    Renderer renderer([&](std::string_view bytes) { output += bytes; });
    renderer.viewport({3, 3}); renderer.configure({23, 13}, {8, 6});
    require(output == "\033[8;3;3t", "resize must precede graphics"); output.clear();
    std::vector<uint8_t> data(13 * 100, 0);
    Frame frame{data.data(), {23, 13}, 100, 2, 1, 0};
    require(renderer.paint(frame) == 9, "initial full paint");
    require(count(output, "a=T") == 9 && output.find(",i=9,p=1,c=1,r=1,C=1,q=2,") != std::string::npos, "one image per cell");
    output.clear(); require(renderer.paint(frame) == 0 && output.empty(), "idle frame emits nothing");
    renderer.damage(0, 0, 23, 13);
    require(renderer.paint(frame) == 0 && output.empty(), "unchanged reported damage emits nothing");
    data[3] = 90; renderer.damage(0, 0, 1, 1);
    require(renderer.paint(frame) == 0, "ignored X byte");
    data[2] = 255; renderer.damage(0, 0, 1, 1);
    require(renderer.paint(frame) == 1 && output.find("; /w") == std::string::npos &&
            output.find(";/wAA") != std::string::npos, "BGRX conversion and one-cell update");
    output.clear(); data[7 * 4] = 42; data[8 * 4] = 42;
    renderer.damage(7, 0, 2, 1);
    require(renderer.paint(frame) == 2 && count(output, "a=T") == 2, "damage crossing cell boundary");
    output.clear(); renderer.damage(INT_MAX, INT_MAX, INT_MAX, INT_MAX);
    renderer.damage(-10, -10, 2, 2); renderer.damage(0, 0, 0, 1);
    require(renderer.paint(frame) == 0 && output.empty(), "damage clips without integer overflow");
    data[12 * 100 + 22 * 4 + 1] = 254; renderer.damage(22, 12, 1, 1);
    require(renderer.paint(frame) == 1 && output.find(",i=9,") != std::string::npos, "partial bottom-right cell");
    output.clear(); renderer.viewport({1, 1}); require(renderer.paint(frame) == 1, "viewport repaint");
    renderer.viewport({3, 3}); require(renderer.paint(frame) == 9, "restore clipped placements");
    output.clear(); renderer.configure({8, 6}, {8, 6});
    require(count(output, "a=d,d=I,i=") == 9 && output.substr(output.size() - 8) == "\033[8;1;1t", "resize deletes owned images and requests new cells");
    bool rejected = false;
    try { Renderer limited([](std::string_view){}, 4); limited.configure({23,13},{8,6}); } catch (...) { rejected = true; }
    require(rejected, "tile capacity checked");
}
void cursor_tests() {
    Renderer renderer([](std::string_view){});
    renderer.viewport({3,1}); renderer.configure({6,2},{2,2});
    std::vector<uint8_t> pixels(6 * 2 * 4);
    Frame frame{pixels.data(), {6,2}, 24, 0,1,2}; renderer.paint(frame);
    renderer.cursor_shape({1,1},{}, std::string("\xff\xff\xff\xff", 4));
    require(renderer.paint(frame) == 1, "cursor touches one cell");
    renderer.cursor_position(4,0);
    require(renderer.paint(frame) == 2, "cursor movement restores old cell and paints new cell");
    renderer.cursor_shape({}, {}, {}); require(renderer.paint(frame) == 1, "hidden cursor restores pixels");
    renderer.cursor_shape({300,1}, {INT_MAX,INT_MIN}, std::string(300*4, '\xff'));
    renderer.cursor_position(INT_MIN, INT_MAX);
    require(renderer.paint(frame) == 0, "large offscreen cursor arithmetic does not overflow");
    renderer.cursor_shape({300,1}, {}, std::string(300*4, '\xff'));
    renderer.cursor_position(0,0);
    require(renderer.paint(frame) == 3, "cursor beyond former width limit renders all affected cells");
}
void storage_tests() {
    require(grid_size({40000,2000}, {1000,1000}) == Size{40,2}, "desktop and cells exceed former arbitrary limits");
    Renderer renderer([](std::string_view){});
    renderer.configure({513,2}, {513,2}); renderer.viewport({1,1});
    std::vector<uint8_t> pixels(513*2*4);
    Frame frame{pixels.data(), {513,2}, 513*4, 0,1,2};
    require(renderer.paint(frame) == 1, "cell exceeding former limit paints completely");
    frame.stride = std::numeric_limits<size_t>::max();
    bool rejected = false;
    try { renderer.paint(frame); } catch (const std::length_error&) { rejected = true; }
    require(rejected, "overflowing framebuffer stride rejected before access");
    renderer.damage(INT64_MIN, INT64_MIN, INT_MAX, INT_MAX);
    renderer.damage(INT64_MAX, INT64_MAX, INT_MAX, INT_MAX);
}
void chunk_tests() {
    std::string output;
    Renderer renderer([&](std::string_view bytes){ output += bytes; });
    renderer.viewport({1,1}); renderer.configure({64,64},{64,64}); output.clear();
    std::vector<uint8_t> pixels(64 * 64 * 4);
    require(renderer.paint({pixels.data(),{64,64},256,0,1,2}) == 1, "large cell paint");
    require(count(output, "\033_G") == 4 && count(output, "m=1;") == 3 && count(output, "m=0;") == 1, "4096-byte chunking");
    require(count(output, "a=T") == 1, "continuations only carry m");
}
void magnification_tests() {
    require(grid_size({3,2},{4,5},3) == Size{3,2}, "magnified ceil geometry");
    for (int factor : {0, -1, INT_MAX}) {
        bool rejected = false;
        try { grid_size({3,2},{4,5},factor); } catch (...) { rejected = true; }
        require(rejected, "invalid or overflowing magnification rejected");
    }
    bool rejected = false;
    try { Renderer limited([](std::string_view){}, 5); limited.configure({3,2},{4,5},3); }
    catch (...) { rejected = true; }
    require(rejected, "magnified tile capacity checked");
    Renderer renderer([](std::string_view){});
    renderer.viewport({3,2}); renderer.configure({3,2},{4,5},3);
    std::vector<uint8_t> pixels(3 * 2 * 4);
    Frame frame{pixels.data(),{3,2},12,0,1,2};
    require(renderer.paint(frame) == 6, "magnified initial paint");
    pixels[(1 * 3 + 1) * 4] = 80;
    renderer.damage(1,1,1,1);
    require(renderer.paint(frame) == 4, "one enlarged pixel damages four non-aligned cells");
    renderer.damage(1,1,1,1);
    require(renderer.paint(frame) == 0, "magnified redundant damage emits nothing");
    renderer.cursor_position(1,1);
    renderer.cursor_shape({1,1},{},std::string("\xff\xff\xff\xff",4));
    require(renderer.paint(frame) == 4, "cursor enlarges across cell boundaries");
    renderer.cursor_position(2,0);
    require(renderer.paint(frame) == 5, "magnified cursor restores old cells");
    renderer.cursor_shape({}, {}, {});
    require(renderer.paint(frame) == 2, "magnified hidden cursor restores cells");
}
void input_tests() {
    std::vector<Event> events; Input input([&](const Event& event){ events.push_back(event); });
    const std::string bytes = "\033[6;18;9t\033[8;40;80t\033[?1016;2$y\033[<0;9;18M\033[<0;9;18m"
        "\033[97;5:1u\033[97;5:3u\033[1;2:3A\033[15;1:2~\033[97:65;2u\xc3\xa9";
    for (char byte : bytes) input.feed(std::string_view(&byte,1));
    require(events.size() == 11, "fragmented input");
    require(events[0].type == EventType::cell_size && events[0].size == Size{9,18}, "cell report");
    require(events[1].size == Size{80,40}, "cell count report");
    require(events[2].type == EventType::mouse_mode && events[2].action == 2, "pixel mouse support");
    require(events[3].x == 8 && events[3].y == 17 && events[4].action == 3, "SGR mouse press and release");
    require(events[5].key == 'a' && events[5].modifiers == 4 && events[6].action == 3, "kitty press and release");
    require(events[7].key == uint32_t(Key::up) && events[7].action == 3, "functional key release");
    require(events[8].key == uint32_t(Key::f5) && events[8].action == 2, "functional key repeat");
    require(events[9].key == 'A' && events[10].key == 233, "shifted text and UTF-8");
    events.clear(); input.feed("\033[<28;2;3M\033[<60;4;5M\033[<28;4;5m\033[<35;4;5M");
    require(events.size() == 4 && events[0].modifiers == 7 && events[1].modifiers == 7 &&
            events[1].button == 60 && events[2].action == 3 && events[3].button == 35,
            "mouse modifiers, drag, release and hover");
    events.clear(); input.feed("\033_Gi=2147483647;OK\033\\\033P>|terminal\033\\\033[?15u\033[?62;4c");
    require(events.empty(), "terminal replies do not become keystrokes");
    input.feed("\033"); require(input.pending_escape(), "standalone escape waits"); input.flush_escape();
    require(events.size() == 1 && events[0].key == uint32_t(Key::escape), "escape timeout");
    events.clear(); input.feed("\033[93;5u"); require(events.size() == 1 && events[0].type == EventType::quit, "local quit");
    events.clear(); input.feed("\033[999999999999999999999u\033[55296u\xc0\x80");
    require(events.empty(), "malformed Unicode and overflow rejected");
    input.feed("\033[48;5;10;17;23t");
    require(events.size() == 3 && events[0].type == EventType::terminal_size && events[0].size == Size{10,5} &&
            events[1].type == EventType::cell_size && events[1].size == Size{2,3} &&
            events[2].type == EventType::pixel_size && events[2].size == Size{23,17}, "in-band resize preserves exact pixels");
    events.clear();
    input.feed("\033[48;0;0;0;0t");
    require(events.empty(), "minimized window keeps the last usable geometry");
    input.feed("\033[" + std::string(9000, ';')); input.feed("a");
    require(events.empty(), "long control sequence never leaks into remote keystrokes");
    input.feed("b");
    require(events.size() == 1 && events[0].key == 'b', "input resumes after long control sequence");
    events.clear();
    const std::string reply = "\033_Gi=" + std::to_string(hdr_query_id) + ";OK\033\\";
    for (char byte : reply) input.feed(std::string_view(&byte, 1));
    require(events.size() == 1 && events[0].type == EventType::hdr_format, "fragmented HDR confirmation");
    events.clear();
    input.feed("\033]" + std::string(20000, 'x') + "\033\\");
    input.feed("\033_Gi=" + std::to_string(hdr_query_id) + ";EINVAL\033\\");
    require(events.empty(), "large OSC and rejected HDR queries never become keys");
}
void hdr_tests() {
    for (unsigned bits = 0; bits < 65536; ++bits) {
        const uint8_t source[] = {uint8_t(bits), uint8_t(bits >> 8)};
        if ((bits & 0x7c00) == 0x7c00) {
            bool rejected = false;
            try { half_read(source); } catch (...) { rejected = true; }
            require(rejected, "non-finite half rejected"); continue;
        }
        char encoded[2]; half_write(encoded, half_read(source));
        require(uint8_t(encoded[0]) == source[0] && uint8_t(encoded[1]) == source[1], "all finite half values round-trip");
    }
    std::string output;
    Renderer renderer([&](std::string_view bytes){ output += bytes; });
    renderer.viewport({2,1}); renderer.configure({2,1},{1,1}); renderer.hdr_output(true);
    std::string pixels(16, '\0');
    for (int i = 0; i < 2; ++i) {
        half_write(pixels.data()+i*8, 4);
        half_write(pixels.data()+i*8+2, .5f + i/2048.f);
        half_write(pixels.data()+i*8+4, -.25f);
        half_write(pixels.data()+i*8+6, 1);
    }
    Frame frame{reinterpret_cast<const uint8_t*>(pixels.data()), {2,1}, 16, 0,1,2, PixelFormat::linear_rgba16};
    output.clear(); require(renderer.paint(frame) == 2, "HDR initial paint");
    require(count(output, "f=64") == 2, "HDR negotiated pixel format");
    for (int i = 0; i < 2; ++i)
        require(output.find(base64(std::string_view(pixels).substr(i*8,8))) != std::string::npos, "HDR values, negative gamut and sub-8-bit precision retained");
    output.clear(); renderer.damage(0,0,2,1);
    require(renderer.paint(frame) == 0 && output.empty(), "HDR unchanged cells suppressed");
    renderer.cursor_shape({1,1},{},std::string("\xff\xff\xff\x80",4));
    require(renderer.paint(frame) == 1, "HDR cursor damages its own cell");
    renderer.cursor_shape({}, {}, {});
    renderer.hdr_output(false); output.clear();
    require(renderer.paint(frame) == 2 && count(output,"f=24") == 2, "SDR fallback never emits f64");
    output.clear(); half_write(pixels.data(), 8); renderer.damage(0,0,1,1);
    require(renderer.paint(frame) == 2, "global SDR tone mapping repaints other cells when peak changes");
}
int main() {
    try { hdr_tests(); render_tests(); cursor_tests(); storage_tests(); chunk_tests(); magnification_tests(); input_tests(); std::cout << "core tests passed\n"; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
