// Copyright 2010-2012 RethinkDB, all rights reserved.
#include "arch/io/io_utils.hpp"

#ifdef _WIN32
#include "windows.hpp"
#else
#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

#include <sys/uio.h>
#include <unistd.h>
#include <string.h>

#include <algorithm>

#include "logger.hpp"
#include "utils.hpp"

#ifndef _WIN32
int _gettid() {
    return syscall(SYS_gettid);
}
#endif

fd_t scoped_fd_t::reset(fd_t f2) {
    if (fd != INVALID_FD) {
#ifdef _WIN32
        BOOL res = CloseHandle(fd);
        guarantee_winerr(res != 0, "CloseHandle failed");
#else
        int res;
        do {
            res = close(fd);
        } while (res != 0 && get_errno() == EINTR);
        guarantee_err(res == 0, "error returned by close(2)");
#endif
    }
    fd = f2;
    return f2;
}

// This completely fills dest_vecs with data from source_vecs (starting at
// offset_into_source).  Crashes if source_vecs is too small!
void fill_bufs_from_source(iovec *dest_vecs, const size_t dest_size,
                           iovec *source_vecs, const size_t source_size,
                           const size_t offset_into_source) {

    // dest_byte always less than dest_vecs[dest_buf].iov_len. Same is true of
    // source.
    size_t dest_buf = 0;
    size_t dest_byte = 0;
    size_t source_buf = 0;
    size_t source_byte = 0;

    // Walk source forward to the offset.
    for (size_t n = offset_into_source; ; ) {
        guarantee(source_buf < source_size);
        if (n < source_vecs[source_buf].iov_len) {
            source_byte = n;
            break;
        } else {
            n -= source_vecs[source_buf].iov_len;
            ++source_buf;
        }
    }

    // Copy bytes until dest is filled.
    while (dest_buf < dest_size) {
        guarantee(source_buf < source_size);
        size_t copy_size = std::min(dest_vecs[dest_buf].iov_len - dest_byte,
                                    source_vecs[source_buf].iov_len - source_byte);

        memcpy(static_cast<char *>(dest_vecs[dest_buf].iov_base) + dest_byte,
               static_cast<char *>(source_vecs[source_buf].iov_base) + source_byte,
               copy_size);

        dest_byte += copy_size;
        if (dest_byte == dest_vecs[dest_buf].iov_len) {
            ++dest_buf;
            dest_byte = 0;
        }

        source_byte += copy_size;
        if (source_byte == source_vecs[source_buf].iov_len) {
            ++source_buf;
            source_byte = 0;
        }
    }
}

namespace io_utils {

scoped_fd_t create_file(const char *filepath) {
    scoped_fd_t fd;
#ifdef _WIN32
    HANDLE h = CreateFile(filename.path().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    fd.reset(h);

    if (fd.get() == INVALID_FD) {
        throw std::runtime_error(strprintf("Failed to open file '%s': %s",
                                           filepath,
                                           winerr_string(GetLastError()).c_str()).c_str());
    }
#else
    int res;
    do {
        res = ::open(filepath, O_WRONLY|O_APPEND|O_CREAT, 0644);
    } while (res == INVALID_FD && get_errno() == EINTR);

    fd.reset(res);

    if (fd.get() == INVALID_FD) {
        throw std::runtime_error(strprintf("Failed to open file '%s': %s",
                                           filepath,
                                           errno_string(errno).c_str()).c_str());
    }
#endif
    return fd;
}

scoped_fd_t open_file_for_read(const char *filepath, std::string *error_out) noexcept {
    scoped_fd_t fd;
#ifdef _WIN32
    fd.reset(CreateFile(filepath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL));
    if (fd.get() == INVALID_FD) {
        *error_out = winerr_string(GetLastError());
    }
#else
    do {
        fd.reset(open(filepath, O_RDONLY));
    } while (fd.get() == INVALID_FD && get_errno() == EINTR);
    if (fd.get() == INVALID_FD) {
        *error_out = errno_string(get_errno());
    }
#endif
    return fd;
}

bool write_all(fd_t fd, const void *data, size_t count, std::string *error_out) noexcept {
#ifdef _WIN32
    DWORD bytes_written;
    BOOL res = WriteFile(fd, data, count, &bytes_written, nullptr);
    if (!res) {
        *error_out = winerr_string(GetLastError()));
        return false;
    }
    return true;
#else
    const char *cdata = static_cast<const char *>(data);
    for (;;) {
        ssize_t write_res = ::write(fd, cdata, count);
        if (write_res < 0) {
            int errsv = get_errno();
            if (errsv == EINTR) {
                continue;
            }
            *error_out = errno_string(errsv);
            return false;
        }

        cdata += write_res;
        count -= write_res;
        if (count == 0) {
            return true;
        }
    }
#endif
}

}  // io_utils