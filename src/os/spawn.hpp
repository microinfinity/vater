#pragma once
/// @file spawn.hpp
/// @brief L1 OS Layer — Login Shell & Command Spawner (fork + setsid + TIOCSCTTY + close_range + execve).

#include "os/pty.hpp"

#include <string>
#include <sys/types.h>
#include <vector>

namespace vater::os {

struct SpawnConfig {
    std::string program;                  // Empty => $SHELL or /bin/sh
    std::vector<std::string> args;        // Additional args (if empty & default shell => login shell "-shell")
    std::string working_dir;              // Empty => inherit
    std::string term{"xterm-256color"};   // Default per decision #1 (configurable to "vater")
    bool login_shell{true};
    std::vector<std::pair<std::string, std::string>> extra_env;
};

struct ChildProcess {
    pid_t pid{-1};
    int exit_status{0};
    bool exited{false};

    [[nodiscard]] bool is_running() const noexcept { return pid > 0 && !exited; }

    /// Non-blocking waitpid(WNOHANG) check. Returns true if child has exited.
    bool poll_exit();

    /// Sends SIGHUP then SIGTERM to the child process group.
    void terminate();
};

/// Forks and execs a child attached to `pty`'s slave end.
bool spawn_in_pty(const Pty& pty, const SpawnConfig& cfg, ChildProcess& out_child);

/// Resolves the user's preferred shell ($SHELL -> getpwuid -> /bin/sh).
[[nodiscard]] std::string resolve_default_shell();

} // namespace vater::os

