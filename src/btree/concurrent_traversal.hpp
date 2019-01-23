// Copyright 2010-2014 RethinkDB, all rights reserved.
#ifndef BTREE_CONCURRENT_TRAVERSAL_HPP_
#define BTREE_CONCURRENT_TRAVERSAL_HPP_

#include "btree/types.hpp"
#include "concurrency/interruptor.hpp"
#include "containers/archive/archive.hpp"

class key_range_t;
namespace rocksdb {
    class Snapshot;
}
namespace rockstore { class store; }

enum class direction_t {
    forward,
    backward,
};

ARCHIVE_PRIM_MAKE_RANGED_SERIALIZABLE(direction_t, int8_t, direction_t::forward, direction_t::backward);


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

continue_bool_t rocks_traversal(
        rockstore::store *rocks,
        const rocksdb::Snapshot *rocksnap,
        const std::string &rocks_kv_prefix,
        const key_range_t &range,
        direction_t direction,
        rocks_traversal_cb *cb);

#endif  // BTREE_CONCURRENT_TRAVERSAL_HPP_
