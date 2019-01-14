// Copyright 2010-2014 RethinkDB, all rights reserved.
#ifndef BTREE_CONCURRENT_TRAVERSAL_HPP_
#define BTREE_CONCURRENT_TRAVERSAL_HPP_

#include "btree/types.hpp"
#include "concurrency/interruptor.hpp"
#include "containers/archive/archive.hpp"

class key_range_t;
namespace rocksdb {
    class Snapshot;
    class OptimisticTransactionDB;
}
namespace rockstore { class store; }
class superblock_t;

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

// TODO: Move this stuff into rockstore/store.hpp
struct rocks_snapshot {
    rocks_snapshot(
        rocksdb::OptimisticTransactionDB *_db,
        const rocksdb::Snapshot *_snapshot) : db(_db), snapshot(_snapshot) {}
    ~rocks_snapshot();
    rocks_snapshot(rocks_snapshot &&movee) : db(movee.db), snapshot(movee.snapshot) {
        movee.db = nullptr;
        movee.snapshot = nullptr;
    }

    void reset();

    rocksdb::OptimisticTransactionDB *db;
    const rocksdb::Snapshot *snapshot;
    DISABLE_COPYING(rocks_snapshot);
};

rocks_snapshot make_snapshot(rockstore::store *rocks);


// TODO: This should freaking take an interruptor, no?

continue_bool_t rocks_traversal(
        rockstore::store *rocks,
        const rocksdb::Snapshot *rocksnap,
        const std::string &rocks_kv_prefix,
        const key_range_t &range,
        direction_t direction,
        rocks_traversal_cb *cb);

// We release the superblock after calling get_snapshot (or starting an iterator, or
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
