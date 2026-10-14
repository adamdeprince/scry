// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Adam DePrince
// Test-only Unix connect replacement. All production launcher/session code is
// compiled unchanged; the RFB peer uses an inherited socketpair so the suite
// also runs in sandboxes that prohibit listening sockets.
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

extern "C" int connect(int socket, const sockaddr* address, socklen_t) {
    const char* descriptor = getenv("GOBLIN_TEST_CONNECT_FD");
    if (!descriptor || address->sa_family != AF_UNIX) { errno = EACCES; return -1; }
    const auto* path = reinterpret_cast<const sockaddr_un*>(address);
    if (access(path->sun_path, F_OK) != 0) { errno = ENOENT; return -1; }
    if (dup2(std::atoi(descriptor), socket) < 0) return -1;
    return fcntl(socket, F_SETFD, FD_CLOEXEC);
}
