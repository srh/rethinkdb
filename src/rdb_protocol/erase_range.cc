// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "rdb_protocol/erase_range.hpp"

#include "buffer_cache/alt.hpp"
#include "btree/concurrent_traversal.hpp"
#include "btree/operations.hpp"
#include "btree/reql_specific.hpp"
#include "btree/types.hpp"
#include "concurrency/promise.hpp"
#include "rdb_protocol/btree.hpp"
#include "rdb_protocol/serialize_datum_onto_blob.hpp"
#include "rdb_protocol/store.hpp"

class collect_keys_helper_t : public rocks_traversal_cb {
public:
    collect_keys_helper_t(const key_range_t &key_range,
                          uint64_t max_keys_to_collect, /* 0 = unlimited */
                          signal_t *interruptor)
        : aborted_(false),
          key_range_(key_range),
          max_keys_to_collect_(max_keys_to_collect),
          interruptor_(interruptor) {
        if (max_keys_to_collect_ != 0) {
            collected_keys_.reserve(max_keys_to_collect_);
        }
    }

    // TODO: We don't use value, can we lazy-load it?
    continue_bool_t handle_pair(
            std::pair<const char *, size_t> key,
            UNUSED std::pair<const char *, size_t> value) override {
        guarantee(!aborted_);
        store_key_t skey(key.second, reinterpret_cast<const uint8_t *>(key.first));
        guarantee(key_range_.contains_key(skey.data(), skey.size()));
        collected_keys_.push_back(skey);
        if (collected_keys_.size() == max_keys_to_collect_ ||
                interruptor_->is_pulsed()) {
            aborted_ = true;
            return continue_bool_t::ABORT;
        } else {
            return continue_bool_t::CONTINUE;
        }
    }

    const std::vector<store_key_t> &get_collected_keys() const {
        return collected_keys_;
    }

    bool get_aborted() const {
        return aborted_;
    }

private:
    std::vector<store_key_t> collected_keys_;
    bool aborted_;

    key_range_t key_range_;
    uint64_t max_keys_to_collect_;
    signal_t *interruptor_;

    DISABLE_COPYING(collect_keys_helper_t);
};

continue_bool_t rdb_erase_small_range(
        rockshard rocksh,
        btree_slice_t *btree_slice,
        const key_range_t &key_range,
        real_superblock_lock *superblock,
        signal_t *interruptor,
        uint64_t max_keys_to_erase,
        std::vector<rdb_modification_report_t> *mod_reports_out,
        key_range_t *deleted_out) {
    rassert(mod_reports_out != nullptr);
    rassert(deleted_out != nullptr);
    mod_reports_out->clear();
    *deleted_out = key_range_t::empty();

    superblock->write_acq_signal()->wait_lazily_ordered();

    // TODO: Use a rocks iterator, delete the keys while iterating.

    /* Step 1: Collect all keys that we want to erase using a depth-first traversal. */
    collect_keys_helper_t key_collector(key_range, max_keys_to_erase,
        interruptor);
    std::string rocks_kv_prefix =
        rockstore::table_primary_prefix(rocksh.table_id);

    // Acquire read lock on superblock before taking snapshot.
    superblock->read_acq_signal()->wait_lazily_ordered();
    rockstore::snapshot snap = make_snapshot(rocksh.rocks);

    // TODO: All btree_depth_first_traversals took an interruptor.  And that got
    // dropped for rocks_traversal.  Check all callers of rocks_traversal to see
    // what to do about that -- here, key_collector takes the interruptor and
    // uses it to abort the concurrent traversal.
    rocks_traversal(
        rocksh.rocks, snap.snap, rocks_kv_prefix, key_range, direction_t::forward,
        &key_collector);
    if (interruptor->is_pulsed()) {
        /* If the interruptor is pulsed during the traversal, then the traversal
        will stop early but not throw an exception. So we have to throw it here.
        */
        throw interrupted_exc_t();
    }

    /* Step 2: Erase each key individually and create the corresponding
       modification reports. */
    for (const auto &key : key_collector.get_collected_keys()) {
        btree_slice->stats.pm_keys_set.record();
        btree_slice->stats.pm_total_keys_set += 1;

        std::string rocks_kv_location = rockstore::table_primary_key(rocksh.table_id, key_to_unescaped_str(key));
        std::pair<std::string, bool> maybe_value
            = rocksh.rocks->try_read(rocks_kv_location);

        // We're still holding a write lock on the superblock, so if the value
        // disappeared since we've populated key_collector, something fishy
        // is going on.
        guarantee(maybe_value.second);

        // The mod_report we generate is a simple delete. While there is generally
        // a difference between an erase and a delete (deletes get backfilled,
        // while an erase is as if the value had never existed), that
        // difference is irrelevant in the case of secondary indexes.
        rdb_modification_report_t mod_report;
        mod_report.primary_key = key;
        // Get the full data
        mod_report.info.deleted.first = datum_deserialize_from_vec(maybe_value.first.data(), maybe_value.first.size());
        mod_reports_out->push_back(mod_report);

        // TODO: repli_timestamp_t::invalid, delete_mode_t::ERASE was used.
        // Maybe kv_location_delete should be called, for good form?
        // Erase the entry from rocksdb.
        superblock->wait_write_batch()->Delete(rocks_kv_location);

        guarantee(key >= deleted_out->right.key());
        *deleted_out = key_range_t(key_range_t::closed, key_range.left,
                                   key_range_t::closed, key);

        if (interruptor->is_pulsed()) {
            /* Note: We have to check the interruptor at the beginning or the end of the
            loop. If we check it in the middle, we might leave the B-tree in a half-
            consistent state, or we might leave `*deleted_out` in a state not consistent
            with what we actually deleted. */
            throw interrupted_exc_t();
        }
    }

    /* If we're done, then set `*deleted_out` to be exactly the same as the range we were
    supposed to delete. This isn't redundant because there may be a gap between the last
    key we actually deleted and the true right-hand side of `key_range`. */
    if (!key_collector.get_aborted()) {
        *deleted_out = key_range;
    }

    /* If we aborted `btree_depth_first_traversal()`, then that's because we found the
    maximum number of keys, so `rdb_erase_small_range()` should be called again to keep
    deleting. If we didn't abort, that's because we hit the right-hand side of the range,
    so `rdb_erase_small_range()` shouldn't be called again. */
    return key_collector.get_aborted()
        ? continue_bool_t::CONTINUE : continue_bool_t::ABORT;
}