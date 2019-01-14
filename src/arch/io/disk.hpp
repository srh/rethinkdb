// Copyright 2010-2013 RethinkDB, all rights reserved.
#ifndef ARCH_IO_DISK_HPP_
#define ARCH_IO_DISK_HPP_

#include "arch/io/io_utils.hpp"
#include "arch/types.hpp"
#include "concurrency/auto_drainer.hpp"
#include "containers/scoped.hpp"
#include "utils.hpp"

#include "perfmon/core.hpp"

namespace rockstore {
class store;
}

// TODO: Remove these unused(?) options.
// The maximum concurrent IO requests per event queue.. the default value.
const int DEFAULT_MAX_CONCURRENT_IO_REQUESTS = 64;

// The maximum user-specifiable value how many concurrent I/O requests may be done per event
// queue.  (A million is a ridiculously high value, but also safely nowhere near INT_MAX.)
const int MAXIMUM_MAX_CONCURRENT_IO_REQUESTS = MILLION;

class io_backender_t : public home_thread_mixin_debug_only_t {
public:
    // This takes what is effectively a global flag whether to use O_DIRECT here.  Nothing technical
    // stops us from specifying this on a file-by-file basis, but right now there's no desire for
    // that.  See https://github.com/rethinkdb/rethinkdb/issues/97#issuecomment-19778177 .
    io_backender_t(rockstore::store *_rocks,
                   file_direct_io_mode_t direct_io_mode,
                   int max_concurrent_io_requests = DEFAULT_MAX_CONCURRENT_IO_REQUESTS);
    ~io_backender_t();
    rockstore::store *rocks() { return rocks_; }
    file_direct_io_mode_t get_direct_io_mode() const;

protected:
    const file_direct_io_mode_t direct_io_mode;
    rockstore::store *rocks_;
    perfmon_collection_t stats;

private:
    DISABLE_COPYING(io_backender_t);
};

// Makes blocking syscalls.  Upon error, returns the errno value.
int perform_datasync(fd_t fd);

// TODO: Might we need to fsync the parent of the rockstore directory?
// Calls fsync() on the parent directory of the given path.
// Returns the errno value in case of an error and 0 otherwise.
MUST_USE int fsync_parent_directory(const char *path);
// Like fsync_parent_directory but warns the user on an error
void warn_fsync_parent_directory(const char *path);

#endif // ARCH_IO_DISK_HPP_

