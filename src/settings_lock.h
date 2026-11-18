// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <filesystem>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

// The launcher and game lock the same stable sidecar before merging and replacing an INI.
class SettingsLock {
public:
    explicit SettingsLock(std::filesystem::path path) {
        path += ".lock";
#ifdef _WIN32
        handle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        OVERLAPPED offset{};
        locked_ = handle_ != INVALID_HANDLE_VALUE && LockFileEx(handle_, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &offset);
#else
        handle_ = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        locked_ = handle_ >= 0 && flock(handle_, LOCK_EX) == 0;
#endif
    }
    ~SettingsLock() {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
#else
        if (handle_ >= 0) close(handle_);
#endif
    }
    SettingsLock(const SettingsLock&) = delete;
    SettingsLock& operator=(const SettingsLock&) = delete;
    explicit operator bool() const { return locked_; }
private:
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int handle_ = -1;
#endif
    bool locked_ = false;
};
