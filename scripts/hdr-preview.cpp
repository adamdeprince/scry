// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Adam DePrince
// A public sample frame through Scry's actual renderer. No desktop capture or
// HPSS network connection is made by this demonstration.
#include "session.hpp"
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
class Preview final : public goblin::Backend {
public:
    Preview(const char* path) {
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("cannot open HDR example frame");
        std::string header(16, '\0'); // SCRYHDR1 + two uint32 little-endian dimensions.
        if (!input.read(header.data(), header.size()) || header.substr(0, 8) != "SCRYHDR1")
            throw std::runtime_error("invalid HDR example header");
        auto dimension = [&](size_t offset) {
            uint32_t value = 0;
            for (unsigned byte = 0; byte < 4; ++byte) value |= uint32_t(uint8_t(header[offset+byte])) << (8*byte);
            if (value > uint32_t(std::numeric_limits<int>::max())) throw std::length_error("HDR dimension overflow");
            return int(value);
        };
        size_ = {dimension(8), dimension(12)};
        goblin::validate_desktop(size_);
        if (size_t(size_.width) > pixels_.max_size() / 8 / size_t(size_.height))
            throw std::length_error("HDR example allocation overflow");
        const size_t bytes = size_t(size_.width) * size_.height * 8;
        if (bytes > size_t(std::numeric_limits<std::streamsize>::max())) throw std::length_error("HDR example read overflow");
        pixels_.resize(bytes);
        if (!input.read(pixels_.data(), std::streamsize(bytes)) || input.peek() != std::char_traits<char>::eof())
            throw std::runtime_error("HDR example has an unexpected data length");
        for (size_t i = 0; i < bytes; i += 2)
            goblin::half_read(reinterpret_cast<const uint8_t*>(pixels_.data()+i));
    }
    void connect(goblin::Session& session, const goblin::Options&) override { session.resize(size_); }
    bool step(int) override { return true; }
    goblin::Frame frame() const override {
        return {reinterpret_cast<const uint8_t*>(pixels_.data()), size_, size_t(size_.width)*8, 0,1,2,
                goblin::PixelFormat::linear_rgba16};
    }
    void key(const goblin::Event&) override {}
    void pointer(int,int,int,int,unsigned) override {}
private:
    goblin::Size size_;
    std::string pixels_;
};
}
int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("usage: scry-hdr-preview FRAME.scryhdr (Ctrl+] to exit)");
        Preview preview(argv[1]);
        goblin::Options options;
        // Reuse HPSS's terminal negotiation, but supply only the public sample.
        options.protocol = "hpss"; options.hdr = "on"; options.view_only = true;
        goblin::Terminal terminal;
        goblin::Session session(terminal, options);
        session.attach(&preview); preview.connect(session, options); session.paint();
        while (!session.stopping()) { session.poll(50); session.paint(); }
        session.attach(nullptr);
    } catch (const std::exception& e) { std::cerr << "HDR preview: " << e.what() << '\n'; return 1; }
}
