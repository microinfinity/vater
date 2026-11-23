/// @file main.cpp
/// @brief Standalone `vaterd` persistent session daemon entrypoint.

#include "mux/daemon/vaterd.hpp"

#include <csignal>
#include <cstdio>
#include <string>

namespace {
vater::daemon::VaterDaemon* g_daemon = nullptr;

void handle_signal(int) {
    if (g_daemon) g_daemon->request_stop();
}
} // namespace

int main(int argc, char** argv) {
    std::string socket_path;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "-s" || arg == "--socket") && i + 1 < argc) {
            socket_path = argv[++i];
        } else if (arg == "-h" || arg == "--help") {
            std::printf("Usage: vaterd [--socket PATH]\n"
                        "Sovereign persistent terminal session daemon for Vater.\n");
            return 0;
        }
    }

    vater::daemon::VaterDaemon daemon(socket_path);
    g_daemon = &daemon;
    ::signal(SIGINT, handle_signal);
    ::signal(SIGTERM, handle_signal);
    ::signal(SIGPIPE, SIG_IGN);

    std::printf("vaterd: listening on %s\n", daemon.socket_path().c_str());
    return daemon.run();
}

