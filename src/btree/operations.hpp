// Copyright 2010-2014 RethinkDB, all rights reserved.
#ifndef BTREE_OPERATIONS_HPP_
#define BTREE_OPERATIONS_HPP_

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
