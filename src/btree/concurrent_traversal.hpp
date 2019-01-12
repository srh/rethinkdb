// Copyright 2010-2014 RethinkDB, all rights reserved.
#ifndef BTREE_CONCURRENT_TRAVERSAL_HPP_
#define BTREE_CONCURRENT_TRAVERSAL_HPP_

#include "btree/depth_first_traversal.hpp"
#include "concurrency/interruptor.hpp"

namespace rockstore { class store; }

// rocks_traversal is basically equivalent to depth first traversal.
class rocks_traversal_cb {
public:
    rocks_traversal_cb() { }
    // The implementor must copy out key and value (if they want to use it) before returning.
    virtual continue_bool_t handle_pair(
            std::pair<const char *, size_t> key, std::pair<const char *, size_t> value)
            THROWS_ONLY(interrupted_exc_t) = 0;
protected:
    virtual ~rocks_traversal_cb() {}
    DISABLE_COPYING(rocks_traversal_cb);
};

// TODO: This should freaking take an interruptor, no?

// We release the superblock after calling get_snapshot (or starting a txn, or
// something) in rocksdb.
continue_bool_t rocks_traversal(
        superblock_t *superblock,
        rockstore::store *rocks,
        const std::string &rocks_kv_prefix,
        const key_range_t &range,
        direction_t direction,
        release_superblock_t release_superblock,
        rocks_traversal_cb *cb);

#endif  // BTREE_CONCURRENT_TRAVERSAL_HPP_
