// Copyright 2010-2013 RethinkDB, all rights reserved.
#include "arch/io/disk.hpp"

#include <fcntl.h>

#ifndef _WIN32
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <libgen.h>
#endif

#include <unistd.h>
#include <limits.h>
#include <stdint.h>

#include <algorithm>
#include <functional>

#include "arch/types.hpp"
#include "arch/runtime/thread_pool.hpp"
#include "arch/runtime/runtime.hpp"
#include "arch/io/disk/filestat.hpp"
#include "backtrace.hpp"
#include "config/args.hpp"
#include "do_on_thread.hpp"
#include "logger.hpp"
#include "math.hpp"
#include "utils.hpp"


// TODO: Beware of _direct_io_mode -- we need to pay attention to this when
// using rockstore (or remove the option).
// TODO: We don't need the max_concurrent_io_requests option anymore, do we?
io_backender_t::io_backender_t(rockstore::store *rocks,
                               file_direct_io_mode_t _direct_io_mode,
                               int max_concurrent_io_requests)
    : direct_io_mode(_direct_io_mode),
      rocks_(rocks) { }

io_backender_t::~io_backender_t() { }

file_direct_io_mode_t io_backender_t::get_direct_io_mode() const { return direct_io_mode; }


// For growing in large chunks at a time.
int64_t chunk_factor(int64_t size, int64_t extent_size) {
    // x is at most 12.5% of size. Overall we align to chunks no larger than 64 extents.
    // This ratio was increased from 6.25% for performance reasons.  Resizing a file
    // (with ftruncate) doesn't actually use disk space, so it's OK if we pick a value
    // that's too big.

    // We round off at an extent_size because it would be silly to allocate a partial
    // extent.
    int64_t x = (size / (extent_size * 8)) * extent_size;
    return clamp<int64_t>(x, extent_size, extent_size * 64);
}

// Upon error, returns the errno value.
int perform_datasync(fd_t fd) {
    // On OS X, we use F_FULLFSYNC because fsync lies.  fdatasync is not available.  On
    // Linux we just use fdatasync.

#ifdef __MACH__

    int fcntl_res;
    do {
        fcntl_res = fcntl(fd, F_FULLFSYNC);
    } while (fcntl_res == -1 && get_errno() == EINTR);

    return fcntl_res == -1 ? get_errno() : 0;

#elif defined(_WIN32)

    BOOL res = FlushFileBuffers(fd);
    if (!res) {
        logWRN("FlushFileBuffers failed: %s", winerr_string(GetLastError()).c_str());
        return EIO;
    }
    return 0;

#elif defined(__linux__)

    int res = fdatasync(fd);
    return res == -1 ? get_errno() : 0;

#else
#error "perform_datasync not implemented"
#endif  // __MACH__
}

MUST_USE int fsync_parent_directory(const char *path) {
    // Locate the parent directory
#ifdef _WIN32
    // TODO WINDOWS
    (void) path;
    return 0;
#else
    char absolute_path[PATH_MAX];
    char *abs_res = realpath(path, absolute_path);
    guarantee_err(abs_res != nullptr, "Failed to determine absolute path for '%s'", path);
    char *parent_path = dirname(absolute_path); // Note: modifies absolute_path

    // Get a file descriptor on the parent directory
    int res;
    do {
        res = open(parent_path, O_RDONLY);
    } while (res == -1 && get_errno() == EINTR);
    if (res == -1) {
        return get_errno();
    }
    scoped_fd_t fd(res);

    do {
        res = fsync(fd.get());
    } while (res == -1 && get_errno() == EINTR);
    if (res == -1) {
        return get_errno();
    }

    return 0;
#endif
}

void warn_fsync_parent_directory(const char *path) {
    int sync_res = fsync_parent_directory(path);
    if (sync_res != 0) {
        logWRN("Failed to sync parent directory of \"%s\" (errno: %d - %s). "
               "You may encounter data loss in case of a system failure. "
               "(Is the file located on a filesystem that doesn't support directory sync? "
               "e.g. VirtualBox shared folders)",
               path, sync_res, errno_string(sync_res).c_str());
    }
}
