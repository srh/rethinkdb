// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "btree/concurrent_traversal.hpp"

#include <stdint.h>

#include <algorithm>
#include <functional>

// TODO: Remove this include if we wrap the iterator stuff...
#include "rocksdb/db.h"
#include "rocksdb/iterator.h"
#include "rocksdb/utilities/optimistic_transaction_db.h"

#include "arch/runtime/coroutines.hpp"
#include "arch/runtime/thread_pool.hpp"
#include "btree/keys.hpp"
#include "btree/operations.hpp"
#include "concurrency/auto_drainer.hpp"
#include "concurrency/semaphore.hpp"
#include "concurrency/fifo_enforcer.hpp"
#include "rockstore/store.hpp"

continue_bool_t process_traversal_element(
        const std::string &rocks_kv_prefix,
        rocksdb::Slice key, rocksdb::Slice value, rocks_traversal_cb *cb) {
    rassert(key.starts_with(rocksdb::Slice(rocks_kv_prefix)));

    key.remove_prefix(rocks_kv_prefix.size());

    return cb->handle_pair(
        std::make_pair(key.data(), key.size()),
        std::make_pair(value.data(), value.size()));
}

continue_bool_t rocks_traversal(
        superblock_t *superblock,
        rockstore::store *rocks,
        const std::string &rocks_kv_prefix,
        const key_range_t &range,
        direction_t direction,
        release_superblock_t release_superblock,
        rocks_traversal_cb *cb) {
    // duh
    rocksdb::OptimisticTransactionDB *db = rocks->db();

    // Acquire read lock on superblock first.
    superblock->read_acq_signal()->wait_lazily_ordered();

    // linux_thread_pool_t::run_in_blocker_pool([&]() {

    // TODO: Check if we use switches for this type in other code.
    if (direction == direction_t::forward) {
        std::string prefixed_left_bound = rocks_kv_prefix + key_to_unescaped_str(range.left);
        std::string prefixed_upper_bound;
        rocksdb::Slice prefixed_upper_bound_slice;
        // TODO: Proper rocksdb::ReadOptions()
        rocksdb::ReadOptions opts;
        if (!range.right.unbounded) {
            prefixed_upper_bound = rocks_kv_prefix + key_to_unescaped_str(range.right.key());
        } else {
            prefixed_upper_bound = rockstore::prefix_end(rocks_kv_prefix);
        }

        if (!prefixed_upper_bound.empty()) {
            // Note: prefixed_upper_bound_slice doesn't copy the string, it points into it.
            prefixed_upper_bound_slice = rocksdb::Slice(prefixed_upper_bound);
            opts.iterate_upper_bound = &prefixed_upper_bound_slice;
        }

        // TODO: Check if we must call NewIterator on the thread pool thread.
        // TODO: Switching threads for every key/value pair is kind of lame.
        scoped_ptr_t<rocksdb::Iterator> iter(db->NewIterator(opts));
        // Release superblock after snapshotted iterator created.
        if (release_superblock == release_superblock_t::RELEASE) {
            superblock->release();
        }
        bool was_valid;
        rocksdb::Slice key_slice;
        rocksdb::Slice value_slice;
        linux_thread_pool_t::run_in_blocker_pool([&]() {
            iter->Seek(prefixed_left_bound);
            was_valid = iter->Valid();
            if (was_valid) {
                key_slice = iter->key();
                value_slice = iter->value();
            }
        });
        while (was_valid) {
            continue_bool_t contbool = process_traversal_element(rocks_kv_prefix, key_slice, value_slice, cb);
            if (contbool == continue_bool_t::ABORT) {
                return continue_bool_t::ABORT;
            }

            linux_thread_pool_t::run_in_blocker_pool([&]() {
                iter->Next();
                was_valid = iter->Valid();
                if (was_valid) {
                    key_slice = iter->key();
                    value_slice = iter->value();
                }
            });
        }
        return continue_bool_t::CONTINUE;
    } else {
        std::string prefixed_left_bound = rocks_kv_prefix + key_to_unescaped_str(range.left);
        // Note: Constructor here doesn't copy the string, it points to it.
        rocksdb::Slice prefixed_left_bound_slice(prefixed_left_bound);
        // TODO: Proper rocksdb::ReadOptions()
        rocksdb::ReadOptions opts;
        opts.iterate_lower_bound = &prefixed_left_bound_slice;
        // TODO: Check if we must call NewIterator on the thread pool thread.
        scoped_ptr_t<rocksdb::Iterator> iter(db->NewIterator(opts));
        // Release superblock after snapshotted iterator created.
        if (release_superblock == release_superblock_t::RELEASE) {
            superblock->release();
        }

        bool was_valid;
        rocksdb::Slice key_slice;
        rocksdb::Slice value_slice;
        linux_thread_pool_t::run_in_blocker_pool([&]() {
            std::string prefixed_right;
            if (!range.right.unbounded) {
                prefixed_right = rocks_kv_prefix + key_to_unescaped_str(range.right.key());
            } else {
                prefixed_right = rockstore::prefix_end(rocks_kv_prefix);
            }

            if (prefixed_right.empty()) {
                iter->SeekToLast();
                was_valid = iter->Valid();
                if (was_valid) {
                    key_slice = iter->key();
                    value_slice = iter->value();
                }
            } else {
                iter->SeekForPrev(prefixed_right);
                was_valid = iter->Valid();
                if (was_valid) {
                    key_slice = iter->key();
                    if (key_slice.ToString() == prefixed_right) {  // TODO: perf
                        iter->Prev();
                        was_valid = iter->Valid();
                        if (was_valid) {
                            key_slice = iter->key();
                            value_slice = iter->value();
                        }
                    } else {
                        value_slice = iter->value();
                    }
                }
            }
        });

        while (was_valid) {
            continue_bool_t contbool = process_traversal_element(rocks_kv_prefix, key_slice, value_slice, cb);
            if (contbool == continue_bool_t::ABORT) {
                return continue_bool_t::ABORT;
            }

            linux_thread_pool_t::run_in_blocker_pool([&]() {
                iter->Prev();
                was_valid = iter->Valid();
                if (was_valid) {
                    key_slice = iter->key();
                    value_slice = iter->value();
                }
            });
        }
        return continue_bool_t::CONTINUE;
    }
}
