// Copyright 2010-2014 RethinkDB, all rights reserved.
#ifndef RDB_PROTOCOL_ERASE_RANGE_HPP_
#define RDB_PROTOCOL_ERASE_RANGE_HPP_

#include <vector>

#include "errors.hpp"
#include "btree/keys.hpp"
#include "btree/types.hpp"
#include "buffer_cache/types.hpp"
#include "containers/uuid.hpp"

class btree_slice_t;
struct rdb_modification_report_t;
class real_superblock_lock;
class rockshard;
class signal_t;

/* `rdb_erase_small_range` has a complexity of O(log n * m) where n is the size of
the btree, and m is the number of documents actually being deleted.

It also requires O(m) memory.

It returns a number of modification reports that should be applied to secondary indexes
separately. Blobs are detached, and should be deleted later if required (passing the
modification reports to store_t::update_sindexes() takes care of that).

Returns `CONTINUE` if it stopped because it collected `max_keys_to_erase` and `ABORT` if
it stopped because it hit the end of the range. */
continue_bool_t rdb_erase_small_range(
    rockshard rocksh,
    btree_slice_t *btree_slice,
    const key_range_t &keys,
    real_superblock_lock *superblock,
    signal_t *interruptor,
    uint64_t max_keys_to_erase /* 0 = unlimited */,
    std::vector<rdb_modification_report_t> *mod_reports_out,
    key_range_t *deleted_out);

#endif  // RDB_PROTOCOL_ERASE_RANGE_HPP_
