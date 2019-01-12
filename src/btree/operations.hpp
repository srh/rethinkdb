// Copyright 2010-2014 RethinkDB, all rights reserved.
#ifndef BTREE_OPERATIONS_HPP_
#define BTREE_OPERATIONS_HPP_

#include <algorithm>
#include <utility>
#include <vector>

#include "btree/node.hpp"
#include "btree/stats.hpp"
#include "buffer_cache/alt.hpp"
#include "concurrency/fifo_enforcer.hpp"
#include "concurrency/new_semaphore.hpp"
#include "concurrency/promise.hpp"
#include "containers/archive/stl_types.hpp"
#include "containers/scoped.hpp"
#include "perfmon/perfmon.hpp"
#include "repli_timestamp.hpp"
#include "utils.hpp"

/* This is the main entry point for performing B-tree operations. */

namespace profile {
class trace_t;
}

<<<<<<< HEAD
class buf_parent_t;
class cache_t;
class value_deleter_t;

||||||| merged common ancestors
class value_deleter_t;

=======
>>>>>>> Remove check and handle split, underfull, get_root code
enum cache_snapshotted_t { CACHE_SNAPSHOTTED_NO, CACHE_SNAPSHOTTED_YES };

/* An abstract superblock provides the starting point for performing B-tree operations.
This makes it so that the B-tree code doesn't actually have to know about the format of
the superblock, or about anything else that might be contained in the superblock besides
the root block ID and the stat block ID. */
// Under rockstore code, this only serves as a read-write lock (possibly, vestigially).
class superblock_t {
public:
    static constexpr std::nullptr_t no_passback = nullptr;

    superblock_t() { }
    virtual ~superblock_t() { }
    // Release the superblock if possible (otherwise do nothing)
    virtual void release() = 0;

    virtual block_id_t get_root_block_id() = 0;
    virtual void set_root_block_id(block_id_t new_root_block) = 0;

    /* If stats collection is desired, create a stat block with `create_stat_block()` and
    store its ID on the superblock, then return it from `get_stat_block_id()`. If stats
    collection is not desired, `get_stat_block_id()` can always return `NULL_BLOCK_ID`.
    */
    virtual block_id_t get_stat_block_id() = 0;

    virtual buf_parent_t expose_buf() = 0;

    cache_t *cache() { return expose_buf().cache(); }

    virtual signal_t *read_acq_signal() = 0;
    virtual signal_t *write_acq_signal() = 0;

private:
    DISABLE_COPYING(superblock_t);
};

class superblock_passback_guard {
public:
    superblock_passback_guard(superblock_t *_superblock, promise_t<superblock_t *> *_pass_back)
        : superblock(_superblock), pass_back_superblock(_pass_back) {}
    ~superblock_passback_guard() {
        if (superblock != nullptr) {
            if (pass_back_superblock != nullptr) {
                pass_back_superblock->pulse(superblock);
            } else {
                superblock->release();
            }
        }
    }
    superblock_t *superblock;
    promise_t<superblock_t *> *pass_back_superblock;
};


/* Create a stat block suitable for storing in a superblock and returning from
`get_stat_block_id()`. */
block_id_t create_stat_block(buf_parent_t parent);

// TODO: Remove or make use of.
/* `delete_mode_t` controls how `apply_keyvalue_change()` acts when `kv_loc->value` is
empty. */
enum class delete_mode_t {
    /* If there was a value before, remove it and add a tombstone. (If `tstamp` is less
    than the cutpoint, no tombstone will be added.) Otherwise, do nothing. This mode is
    used for regular delete queries. */
    REGULAR_QUERY,
    /* If there was a value or tombstone before, remove it. This mode is used for erasing
    ranges of the database (e.g. during resharding) and also sometimes in backfilling. */
    ERASE,
    /* If there was a value or tombstone before, remove it. Then add a tombstone,
    regardless of what was present before, unless `tstamp` is less than the cutpoint.
    This mode is used for transferring tombstones from other servers in backfilling. */
    MAKE_TOMBSTONE
};

#endif  // BTREE_OPERATIONS_HPP_
