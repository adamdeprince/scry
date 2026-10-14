// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Adam DePrince
#include "session.hpp"
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    using namespace goblin;
    try {
        if (argc < 2) throw std::runtime_error("connected socket descriptor required");
        Options options; options.protocol = "vnc"; options.host = "local-test";
        options.password = "test-password";
        for (int i = 2; i < argc; ++i) {
            if (std::string_view(argv[i]) == "--view-only") options.view_only = true;
            else if (std::string_view(argv[i]) == "--magnification" && i + 1 < argc)
                options.magnification = std::stoi(argv[++i]);
            else options.username = argv[i];
        }
        Terminal terminal; Session session(terminal, options);
        auto backend = make_vnc(std::stoi(argv[1]));
        session.attach(backend.get()); backend->connect(session, options);
        session.paint();
        while (!session.stopping()) {
            session.poll();
            if (session.stopping() || !backend->step(20)) break;
            session.paint();
        }
        return 0;
    } catch (const std::exception& error) {
        std::string message = error.what();
        for (char& c : message) if (static_cast<unsigned char>(c) < 32 || c == 127) c = ' ';
        std::cerr << message << '\n'; return 1;
    }
}
