// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <filesystem>
#include <mutex>
#include <system_error>
#if defined(__APPLE__) || defined(__linux__)
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace Common {
// Serialize offline read-modify-write transactions from the library and CLI.
class ProfileActivityLock {
public:
    explicit ProfileActivityLock(const std::filesystem::path& nand) {
#if defined(__APPLE__) || defined(__linux__)
        std::filesystem::create_directories(nand);
        fd = open((nand / "cara-activity.lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        if (fd < 0) throw std::system_error(errno, std::generic_category(), "Open activity lock");
        while (flock(fd, LOCK_EX) < 0) {
            if (errno == EINTR) continue;
            const auto error = errno;
            close(fd);
            throw std::system_error(error, std::generic_category(), "Lock console activity");
        }
#endif
    }
    ~ProfileActivityLock() {
#if defined(__APPLE__) || defined(__linux__)
        close(fd);
#endif
    }
    ProfileActivityLock(const ProfileActivityLock&) = delete;
    ProfileActivityLock& operator=(const ProfileActivityLock&) = delete;
private:
#if defined(__APPLE__) || defined(__linux__)
    int fd = -1;
#else
    inline static std::mutex mutex;
    std::lock_guard<std::mutex> guard{mutex};
#endif
};
}
