// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Adam DePrince
#pragma once
#include "renderer.hpp"
#include <termios.h>

namespace goblin {
class Terminal {
public:
    Terminal();
    ~Terminal();
    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;
    void write(std::string_view bytes);
    void flush();
    std::string read(int timeout_ms);
    Size cells() const;
    Size cell_pixels() const;
    Size pixels() const;
    static bool stopping();
    static bool resized();
private:
    termios saved_{};
    bool raw_ = false, active_ = false;
    std::string output_;
};
}
