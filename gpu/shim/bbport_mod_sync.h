// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#ifdef _WIN32
#include <windows.h>
#endif

// Display-only bridge. Readiness and lobby admission belong to the coordinator;
// no value in this file grants permission to join a match or load an archive.
namespace BbModSync {
struct State {
    std::string phase, label;
    double completed = 0, total = 0, timestamp = 0;
    bool visible = false;
};

inline State Parse(const std::string& text, double now) {
    State state;
    if (text.size() > 1024) return state;
    std::istringstream input(text);
    std::string magic, stamp, done, total, extra;
    if (!std::getline(input, magic) || magic != "CRASHNL_SYNC_1" ||
        !std::getline(input, stamp) || !std::getline(input, state.phase) ||
        !std::getline(input, done) || !std::getline(input, total) ||
        !std::getline(input, state.label) || std::getline(input, extra)) return {};
    auto number = [](const std::string& value, double& out) {
        char* end = nullptr;
        out = std::strtod(value.c_str(), &end);
        return !value.empty() && end == value.c_str() + value.size() && std::isfinite(out);
    };
    if (!number(stamp, state.timestamp) || !number(done, state.completed) ||
        !number(total, state.total) || state.completed < 0 || state.total < state.completed ||
        state.total > 9007199254740992.0 || std::floor(state.total) != state.total ||
        std::floor(state.completed) != state.completed || state.timestamp < 0 ||
        state.timestamp > now + 5 || state.label.size() > 640) return {};
    if (state.phase != "checking" && state.phase != "downloading" &&
        state.phase != "verifying" && state.phase != "ready" &&
        state.phase != "error" && state.phase != "fallback" && state.phase != "hidden") return {};
    state.visible = state.phase != "hidden";
    if (state.phase == "ready" && now - state.timestamp > 3) state.visible = false;
    if (state.phase == "fallback" && now - state.timestamp > 8) state.visible = false;
    if (state.visible && state.phase != "ready" && state.phase != "error" && state.phase != "fallback" &&
        now - state.timestamp > 10) {
        state.phase = "error";
        state.label = "Sync stopped responding; normal fallback remains available.";
    }
    return state;
}

inline State Snapshot() {
    static std::mutex mutex;
    static State state;
    static auto next = std::chrono::steady_clock::time_point{};
    std::lock_guard<std::mutex> lock(mutex);
    const auto tick = std::chrono::steady_clock::now();
    if (tick < next) return state;
    next = tick + std::chrono::milliseconds(100);
    const char* path = std::getenv("BB_MOD_SYNC_STATUS");
    if (!path || !*path) return state = {};
    std::string text;
#ifdef _WIN32
    // The publisher replaces a complete file atomically; permit rename while open.
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, nullptr, 0);
    if (!count) return state = {};
    std::wstring wide(count, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide.data(), count);
    HANDLE file = CreateFileW(wide.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return state = {};
    char buffer[1025];
    DWORD size = 0;
    const bool ok = ReadFile(file, buffer, sizeof(buffer), &size, nullptr) != 0;
    CloseHandle(file);
    if (!ok || size > 1024) return state = {};
    text.assign(buffer, size);
#else
    std::ifstream file(path, std::ios::binary);
    char buffer[1025];
    file.read(buffer, sizeof(buffer));
    text.assign(buffer, static_cast<size_t>(file.gcount()));
#endif
    const double now = std::chrono::duration<double>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return state = Parse(text, now);
}
} // namespace BbModSync
