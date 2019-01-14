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

#include <algorithm>
#include <functional>

#include "arch/types.hpp"
#include "arch/runtime/thread_pool.hpp"
#include "arch/runtime/runtime.hpp"
#include "arch/io/disk/filestat.hpp"
#include "arch/io/disk/pool.hpp"
#include "arch/io/disk/conflict_resolving.hpp"
#include "arch/io/disk/stats.hpp"
#include "arch/io/disk/accounting.hpp"
#include "backtrace.hpp"
#include "config/args.hpp"
#include "do_on_thread.hpp"
#include "logger.hpp"
#include "utils.hpp"


/* Disk manager object takes care of queueing operations, collecting statistics, preventing
   conflicts, and actually sending them to the disk. */
class linux_disk_manager_t : public home_thread_mixin_t {
public:
    struct action_t : public stats_diskmgr_t::action_t {
        action_t(threadnum_t _cb_thread, linux_iocallback_t *_cb)
            : cb_thread(_cb_thread), cb(_cb) { }
        threadnum_t cb_thread;
        linux_iocallback_t *cb;
    };


    linux_disk_manager_t(linux_event_queue_t *queue,
                         int batch_factor,
                         int max_concurrent_io_requests,
                         perfmon_collection_t *stats) :
        stack_stats(stats, "stack"),
        conflict_resolver(stats),
        accounter(batch_factor),
        backend_stats(stats, "backend", accounter.producer),
        backend(queue, backend_stats.producer, max_concurrent_io_requests),
        outstanding_txn(0)
    {
        /* Hook up the `submit_fun`s of the parts of the IO stack that are above the
        queue. (The parts below the queue use the `passive_producer_t` interface instead
        of a callback function.) */
        stack_stats.submit_fun = std::bind(&conflict_resolving_diskmgr_t::submit,
                                           &conflict_resolver, ph::_1);
        conflict_resolver.submit_fun = std::bind(&accounting_diskmgr_t::submit,
                                                 &accounter, ph::_1);

        /* Hook up everything's `done_fun`. */
        backend.done_fun = std::bind(&stats_diskmgr_2_t::done, &backend_stats, ph::_1);
        backend_stats.done_fun = std::bind(&accounting_diskmgr_t::done, &accounter, ph::_1);
        accounter.done_fun = std::bind(&conflict_resolving_diskmgr_t::done,
                                       &conflict_resolver, ph::_1);
        conflict_resolver.done_fun = std::bind(&stats_diskmgr_t::done, &stack_stats, ph::_1);
        stack_stats.done_fun = std::bind(&linux_disk_manager_t::done, this, ph::_1);
    }

    ~linux_disk_manager_t() {
        rassert(outstanding_txn == 0,
                "Closing a file with outstanding txns (%" PRIiPTR " of them)\n",
                outstanding_txn);
    }

    void *create_account(int pri, int outstanding_requests_limit) {
        return new accounting_diskmgr_t::account_t(&accounter, pri, outstanding_requests_limit);
    }

    void destroy_account(void *account) {
        coro_t::spawn_on_thread([account] {
            // The account destructor can block if there are outstanding requests.
            delete static_cast<accounting_diskmgr_t::account_t *>(account);
        }, home_thread());
    }

    void submit_action_to_stack_stats(action_t *a) {
        assert_thread();
        outstanding_txn++;
        stack_stats.submit(a);
    }

    void submit_write(fd_t fd, const void *buf, size_t count, int64_t offset,
                      void *account, linux_iocallback_t *cb,
                      bool wrap_in_datasyncs) {
        threadnum_t calling_thread = get_thread_id();

        action_t *a = new action_t(calling_thread, cb);
        a->make_write(fd, buf, count, offset, wrap_in_datasyncs);
        a->account = static_cast<accounting_diskmgr_t::account_t *>(account);

        do_on_thread(home_thread(),
                     std::bind(&linux_disk_manager_t::submit_action_to_stack_stats, this,
                               a));
    }

    void submit_resize(fd_t fd, int64_t old_size, int64_t new_size,
                      void *account, linux_iocallback_t *cb,
                      bool wrap_in_datasyncs) {
        threadnum_t calling_thread = get_thread_id();

        action_t *a = new action_t(calling_thread, cb);
        a->make_resize(fd, old_size, new_size, wrap_in_datasyncs);
        a->account = static_cast<accounting_diskmgr_t::account_t *>(account);

        do_on_thread(home_thread(),
                     std::bind(&linux_disk_manager_t::submit_action_to_stack_stats, this,
                               a));
    }

#ifndef USE_WRITEV
#error "USE_WRITEV not defined.  Did you include pool.hpp?"
#elif USE_WRITEV
    void submit_writev(fd_t fd, scoped_array_t<iovec> &&bufs, size_t count,
                       int64_t offset, void *account, linux_iocallback_t *cb) {
        threadnum_t calling_thread = get_thread_id();

        action_t *a = new action_t(calling_thread, cb);
        a->make_writev(fd, std::move(bufs), count, offset);
        a->account = static_cast<accounting_diskmgr_t::account_t *>(account);

        do_on_thread(home_thread(),
                     std::bind(&linux_disk_manager_t::submit_action_to_stack_stats, this,
                               a));
    }
#endif  // USE_WRITEV

    void submit_read(fd_t fd, void *buf, size_t count, int64_t offset, void *account, linux_iocallback_t *cb) {
        threadnum_t calling_thread = get_thread_id();

        action_t *a = new action_t(calling_thread, cb);
        a->make_read(fd, buf, count, offset);
        a->account = static_cast<accounting_diskmgr_t::account_t*>(account);

        do_on_thread(home_thread(),
                     std::bind(&linux_disk_manager_t::submit_action_to_stack_stats, this,
                               a));
    }

    void done(stats_diskmgr_t::action_t *a) {
        assert_thread();
        outstanding_txn--;
        action_t *a2 = static_cast<action_t *>(a);
        bool succeeded = a2->get_succeeded();
        if (succeeded) {
            do_on_thread(a2->cb_thread,
                         std::bind(&linux_iocallback_t::on_io_complete, a2->cb));
        } else {
            do_on_thread(a2->cb_thread,
                         std::bind(&linux_iocallback_t::on_io_failure,
                                   a2->cb, a2->get_io_errno(),
                                   static_cast<int64_t>(a2->get_offset()),
                                   static_cast<int64_t>(a2->get_count())));
        }
        delete a2;
    }

private:
    /* These fields describe the entire IO stack. At the top level, we allocate a new
    action_t object for each operation and record its callback. Then it passes through
    the conflict resolver, which enforces ordering constraints between IO operations by
    holding back operations that must be run after other, currently-running, operations.
    Then it goes to the account manager, which queues up running IO operations according
    to which account they are part of. Finally the "backend" pops the IO operations
    from the queue.

    At two points in the process--once as soon as it is submitted, and again right
    as the backend pops it off the queue--its statistics are recorded. The "stack stats"
    will tell you how many IO operations are queued. The "backend stats" will tell you
    how long the OS takes to perform the operations. Note that it's not perfect, because
    it counts operations that have been queued by the backend but not sent to the OS yet
    as having been sent to the OS. */

    stats_diskmgr_t stack_stats;
    conflict_resolving_diskmgr_t conflict_resolver;
    accounting_diskmgr_t accounter;
    stats_diskmgr_2_t backend_stats;
    pool_diskmgr_t backend;


    intptr_t outstanding_txn;

    DISABLE_COPYING(linux_disk_manager_t);
};

io_backender_t::io_backender_t(rockstore::store *rocks,
                               file_direct_io_mode_t _direct_io_mode,
                               int max_concurrent_io_requests)
    : direct_io_mode(_direct_io_mode),
      rocks_(rocks),
      diskmgr(new linux_disk_manager_t(&linux_thread_pool_t::get_thread()->queue,
                                       DEFAULT_IO_BATCH_FACTOR,
                                       max_concurrent_io_requests,
                                       &stats)) { }

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
