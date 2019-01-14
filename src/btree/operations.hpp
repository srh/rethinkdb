// Copyright 2010-2014 RethinkDB, all rights reserved.
#ifndef BTREE_OPERATIONS_HPP_
#define BTREE_OPERATIONS_HPP_

#include <algorithm>
#include <utility>
#include <vector>

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

enum cache_snapshotted_t { CACHE_SNAPSHOTTED_NO, CACHE_SNAPSHOTTED_YES };

/* An abstract superblock provides the starting point for performing B-tree operations.
This makes it so that the B-tree code doesn't actually have to know about the format of
the superblock, or about anything else that might be contained in the superblock besides
the root block ID and the stat block ID. */
// Under rockstore code, this only serves as a read-write lock (possibly, vestigially).
class superblock_t {
public:
    superblock_t() { }
    virtual ~superblock_t() { }

    virtual void release() = 0;
    virtual const signal_t *read_acq_signal() = 0;

private:
    DISABLE_COPYING(superblock_t);
};

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
