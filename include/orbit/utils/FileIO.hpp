#pragma once
// Portable wrappers for the few file calls used to serve file responses.
// These are functions rather than `#define close _close`-style macros, which
// also rewrote every member named close()/open() in the including file.
#include <cstddef>
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#include <stdio.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace utils::file {

/// Opens a file for reading (binary, close-on-exec); -1 on failure.
inline int open_read_only(const char* path) {
#ifdef _WIN32
    return ::_open(path, _O_RDONLY | _O_BINARY);
#else
    return ::open(path, O_RDONLY | O_CLOEXEC);
#endif
}

inline int close(int fd) {
#ifdef _WIN32
    return ::_close(fd);
#else
    return ::close(fd);
#endif
}

/// Reads up to count bytes at offset (64-bit on every platform); -1 on error.
inline long long pread(int fd, void* buf, size_t count, long long offset) {
#ifdef _WIN32
    // Each file response owns its descriptor, so moving its position is safe.
    if (::_lseeki64(fd, offset, SEEK_SET) < 0) return -1;
    return ::_read(fd, buf, static_cast<unsigned int>(count));
#else
    return ::pread(fd, buf, count, static_cast<off_t>(offset));
#endif
}

} // namespace utils::file
