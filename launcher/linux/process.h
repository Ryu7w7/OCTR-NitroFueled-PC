// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cerrno>
#include <atomic>
#include <chrono>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <string>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace LinuxProcess {
struct Child { pid_t pid = 0; int output = -1; };
struct Result { bool started = false; int code = 1; std::string output; };

// argv never passes through a shell. Each child owns a process group, including its descendants.
inline Child Start(const std::vector<std::string>& args, const std::string& cwd,
                   const std::string& input = {}, int input_fd = -1, bool capture = true, char* const* env = nullptr) {
    if (args.empty()) return {};
    int pipe[2];
    if (pipe2(pipe, O_CLOEXEC) != 0) return {};
    int in = input_fd >= 0 ? fcntl(input_fd, F_DUPFD_CLOEXEC, 3) : input.empty() ? open("/dev/null", O_RDONLY | O_CLOEXEC) : memfd_create("launcher-input", MFD_CLOEXEC);
    if (in < 0) { close(pipe[0]); close(pipe[1]); return {}; }
    if (!capture) {
        const int sink = open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (sink < 0) { close(in); close(pipe[0]); close(pipe[1]); return {}; }
        close(pipe[1]); pipe[1] = sink;
    }
    size_t at = 0;
    while (at < input.size()) {
        const auto n = write(in, input.data() + at, input.size() - at);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(in); close(pipe[0]); close(pipe[1]); return {}; }
        at += size_t(n);
    }
    if (!input.empty()) lseek(in, 0, SEEK_SET);
    std::vector<char*> argv;
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, in, STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipe[1], STDERR_FILENO);
    if (!cwd.empty()) posix_spawn_file_actions_addchdir_np(&actions, cwd.c_str());
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0);
    Child child;
    const int error = posix_spawnp(&child.pid, argv[0], &actions, &attr, argv.data(), env ? env : environ);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    close(in);
    close(pipe[1]);
    if (error) { close(pipe[0]); errno = error; return {}; }
    if (capture) child.output = pipe[0]; else close(pipe[0]);
    return child;
}

inline int ExitCode(int status) {
    return WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1;
}

inline Result Run(const std::vector<std::string>& args, const std::string& cwd,
                  unsigned timeout_ms, const std::string& input = {}, const std::atomic<bool>* cancel = nullptr) {
    Child child = Start(args, cwd, input);
    Result result;
    if (!child.pid) return result;
    result.started = true;
    fcntl(child.output, F_SETFL, O_NONBLOCK);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    bool exited = false, eof = false;
    auto cancelled_at = std::chrono::steady_clock::time_point{};
    int status = 0;
    while (!exited || !eof) {
        char buffer[8192];
        ssize_t n;
        while ((n = read(child.output, buffer, sizeof buffer)) > 0) {
            // Bound helper output while continuing to drain its pipe.
            if (result.output.size() < 16 * 1024 * 1024) result.output.append(buffer, size_t(n));
        }
        eof = n == 0;
        if (!exited) exited = waitpid(child.pid, &status, WNOHANG) == child.pid;
        if (exited && eof) break;
        const auto now = std::chrono::steady_clock::now();
        if (cancel && cancel->load() && cancelled_at == std::chrono::steady_clock::time_point{}) {
            cancelled_at = now;
            kill(-child.pid, SIGTERM);
        }
        if (now >= deadline || (cancelled_at != std::chrono::steady_clock::time_point{} &&
                               now - cancelled_at >= std::chrono::seconds(1))) {
            kill(-child.pid, SIGKILL);
            if (!exited) while (waitpid(child.pid, &status, 0) < 0 && errno == EINTR) {}
            result.code = cancel && cancel->load() ? 130 : 124;
            close(child.output);
            return result;
        }
        pollfd fd{child.output, POLLIN, 0};
        poll(&fd, 1, 20);
    }
    close(child.output);
    result.code = ExitCode(status);
    return result;
}

// Only the quoting emitted by Quote() plus literal arguments is accepted; no expansion or execution.
inline std::vector<std::string> Arguments(const std::string& command) {
    std::vector<std::string> args;
    std::string word;
    char quote = 0;
    bool started = false, escape = false;
    for (char c : command) {
        if (escape) { word += c; escape = false; started = true; }
        else if (c == '\\' && quote != '\'') { escape = true; started = true; }
        else if (quote) { if (c == quote) quote = 0; else word += c; }
        else if (c == '\'' || c == '"') { quote = c; started = true; }
        else if (c == ' ' || c == '\t' || c == '\n') {
            if (started) { args.push_back(word); word.clear(); started = false; }
        } else { word += c; started = true; }
    }
    if (quote || escape) return {};
    if (started) args.push_back(word);
    return args;
}
}
