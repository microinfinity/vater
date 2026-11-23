/// @file spawn.cpp
/// @brief L1 OS Layer — Pure POSIX Process Spawner implementation.

#include "os/spawn.hpp"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace vater::os {

namespace {

void close_fds_from(int first_fd) noexcept {
#ifdef SYS_close_range
    if (::syscall(SYS_close_range, static_cast<unsigned int>(first_fd), ~0u, 0u) == 0) {
        return;
    }
#endif
    long max_fd = ::sysconf(_SC_OPEN_MAX);
    if (max_fd < 0 || max_fd > 65536) max_fd = 4096;
    for (int fd = first_fd; fd < static_cast<int>(max_fd); ++fd) {
        ::close(fd);
    }
}

std::string basename_of(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

} // namespace

std::string resolve_default_shell() {
    if (const char* env_sh = std::getenv("SHELL"); env_sh && env_sh[0] != '\0') {
        return env_sh;
    }
    if (struct passwd* pw = ::getpwuid(::getuid()); pw && pw->pw_shell && pw->pw_shell[0] != '\0') {
        return pw->pw_shell;
    }
    return "/bin/sh";
}

bool ChildProcess::poll_exit() {
    if (pid <= 0 || exited) return exited;
    int status = 0;
    pid_t r = ::waitpid(pid, &status, WNOHANG);
    if (r == pid) {
        exited = true;
        if (WIFEXITED(status)) exit_status = WEXITSTATUS(status);
        else if (WIFSIGNALED(status)) exit_status = 128 + WTERMSIG(status);
        else exit_status = 0;
        return true;
    }
    if (r < 0 && errno == ECHILD) {
        exited = true;
        return true;
    }
    return false;
}

void ChildProcess::terminate() {
    if (pid <= 0 || exited) return;
    ::kill(-pid, SIGHUP);
    ::kill(pid, SIGHUP);
    if (!poll_exit()) {
        ::usleep(20000);
        if (!poll_exit()) {
            ::kill(-pid, SIGTERM);
            ::kill(pid, SIGTERM);
            int status = 0;
            ::waitpid(pid, &status, WNOHANG);
        }
    }
}

bool spawn_in_pty(const Pty& pty, const SpawnConfig& cfg, ChildProcess& out_child) {
    if (!pty.is_open()) return false;

    const std::string prog = cfg.program.empty() ? resolve_default_shell() : cfg.program;

    // Build argv before fork (async-signal-safe child)
    std::vector<std::string> argv_storage;
    if (cfg.args.empty() && cfg.login_shell && cfg.program.empty()) {
        argv_storage.push_back("-" + basename_of(prog));
    } else {
        argv_storage.push_back(prog);
        for (const auto& a : cfg.args) argv_storage.push_back(a);
    }
    std::vector<char*> argv_ptrs;
    argv_ptrs.reserve(argv_storage.size() + 1);
    for (auto& s : argv_storage) argv_ptrs.push_back(s.data());
    argv_ptrs.push_back(nullptr);

    // Build sanitized environment before fork
    std::vector<std::string> env_storage;
    auto should_strip = [](const char* entry) -> bool {
        static constexpr const char* kStripPrefixes[] = {
            "TERM=", "COLORTERM=", "TERM_PROGRAM=", "TERM_PROGRAM_VERSION=",
            "VATER_", "COLUMNS=", "LINES=", "TERMCAP="
        };
        for (const char* pfx : kStripPrefixes) {
            if (std::strncmp(entry, pfx, std::strlen(pfx)) == 0) return true;
        }
        return false;
    };

    if (environ) {
        for (char** ep = environ; *ep != nullptr; ++ep) {
            if (!should_strip(*ep)) {
                env_storage.emplace_back(*ep);
            }
        }
    }
    env_storage.push_back("TERM=" + (cfg.term.empty() ? std::string("xterm-256color") : cfg.term));
    env_storage.push_back("COLORTERM=truecolor");
    env_storage.push_back("TERM_PROGRAM=vater");
    env_storage.push_back("TERM_PROGRAM_VERSION=0.1.0");
    for (const auto& [k, v] : cfg.extra_env) {
        env_storage.push_back(k + "=" + v);
    }

    std::vector<char*> envp_ptrs;
    envp_ptrs.reserve(env_storage.size() + 1);
    for (auto& s : env_storage) envp_ptrs.push_back(s.data());
    envp_ptrs.push_back(nullptr);

    const std::string cwd = cfg.working_dir;
    const std::string slave_path = pty.slave_name();

    pid_t pid = ::fork();
    if (pid < 0) return false;

    if (pid == 0) {
        // ── Child Process ──
        // Reset signal mask & handlers to defaults
        sigset_t empty_mask;
        sigemptyset(&empty_mask);
        ::sigprocmask(SIG_SETMASK, &empty_mask, nullptr);
        ::signal(SIGCHLD, SIG_DFL);
        ::signal(SIGHUP, SIG_DFL);
        ::signal(SIGINT, SIG_DFL);
        ::signal(SIGQUIT, SIG_DFL);
        ::signal(SIGTERM, SIG_DFL);
        ::signal(SIGALRM, SIG_DFL);
        ::signal(SIGPIPE, SIG_DFL);

        if (::setsid() < 0) ::_exit(126);

        int sfd = pty.open_slave();
        if (sfd < 0) ::_exit(126);

        if (::ioctl(sfd, TIOCSCTTY, 0) < 0) {
            ::close(sfd);
            ::_exit(126);
        }

        if (::dup2(sfd, STDIN_FILENO) < 0 ||
            ::dup2(sfd, STDOUT_FILENO) < 0 ||
            ::dup2(sfd, STDERR_FILENO) < 0) {
            ::_exit(126);
        }

        close_fds_from(3);

        if (!cwd.empty()) {
            if (::chdir(cwd.c_str()) != 0) {}
        }

        ::execve(prog.c_str(), argv_ptrs.data(), envp_ptrs.data());
        // Fallback: search PATH via execvp if prog was not an absolute path
        if (prog.find('/') == std::string::npos) {
            ::execvpe(prog.c_str(), argv_ptrs.data(), envp_ptrs.data());
        }
        ::_exit(127);
    }

    out_child.pid = pid;
    out_child.exited = false;
    out_child.exit_status = 0;
    return true;
}

} // namespace vater::os
