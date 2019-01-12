// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "btree/backfill.hpp"

#include "rocksdb/utilities/optimistic_transaction_db.h"

#include "arch/runtime/coroutines.hpp"
#include "arch/runtime/thread_pool.hpp"
#include "btree/backfill_debug.hpp"
#include "btree/depth_first_traversal.hpp"
#include "btree/leaf_node.hpp"
#include "btree/reql_specific.hpp"
#include "concurrency/pmap.hpp"
#include "containers/archive/optional.hpp"
#include "containers/archive/stl_types.hpp"
#include "rockstore/rockshard.hpp"
#include "rockstore/store.hpp"

// TODO: Remove
/* `MAX_CONCURRENT_VALUE_LOADS` is the maximum number of coroutines we'll use for loading
values from the leaf nodes. */
// static const int MAX_CONCURRENT_VALUE_LOADS = 16;

continue_bool_t btree_send_backfill_pre(
        real_superblock_t *superblock,
        release_superblock_t release_superblock,
        value_sizer_t *sizer,
        const key_range_t &range,
        repli_timestamp_t reference_timestamp,
        btree_backfill_pre_item_consumer_t *pre_item_consumer,
        signal_t *interruptor) {
    (void)sizer;  // TODO remove param
    (void)reference_timestamp;  // TODO!?
    (void)interruptor;  // TODO remove param
    backfill_debug_range(range, strprintf(
        "btree_send_backfill_pre %" PRIu64, reference_timestamp.longtime));

    superblock->read_acq_signal()->wait_lazily_unordered();
    // We just request the entire range at once, since we lack timestamp data on rocksdb.
    continue_bool_t cont = pre_item_consumer->on_pre_item(backfill_pre_item_t{range});
    if (release_superblock == release_superblock_t::RELEASE) {
        superblock->release();
    }
    return cont;
}

continue_bool_t send_all_in_keyrange(
        rockshard rocksh, real_superblock_t *superblock, release_superblock_t release_superblock,
        const key_range_t &range,
        repli_timestamp_t reference_timestamp, btree_backfill_item_consumer_t *item_consumer,
        backfill_item_memory_tracker_t *memory_tracker,
        signal_t *interruptor) {
    // TODO: Use memory_tracker??
    (void)memory_tracker;
    (void)interruptor;  // TODO: Use interruptor.
    (void)reference_timestamp;  // TODO: Use this?
    rocksdb::OptimisticTransactionDB *db = rocksh.rocks->db();

    std::string rocks_kv_prefix = rockstore::table_primary_prefix(rocksh.table_id, rocksh.shard_no);

    std::string left = rocks_kv_prefix + key_to_unescaped_str(range.left);
    std::string right = range.right.unbounded
        ? rockstore::prefix_end(rocks_kv_prefix)
        : rocks_kv_prefix + key_to_unescaped_str(range.right.key());

    rocksdb::Slice right_slice(right.data(), right.size());

    // rocksdb::ReadOptions()
    rocksdb::ReadOptions opts;
    opts.iterate_upper_bound = &right_slice;

    // TODO: Must NewIterator be in a blocker thread?
    // TODO: With all superblock read_acq_signals... use the interruptor?
    superblock->read_acq_signal()->wait_lazily_unordered();
    repli_timestamp_t max_timestamp = superblock->get()->get_recency();
    scoped_ptr_t<rocksdb::Iterator> iter(db->NewIterator(opts));
    if (release_superblock == release_superblock_t::RELEASE) {
        superblock->release();
    }

    bool was_valid = iter->Valid();
    rocksdb::Slice key_slice;
    rocksdb::Slice value_slice;
    linux_thread_pool_t::run_in_blocker_pool([&]() {
        iter->Seek(left);
        was_valid = iter->Valid();

        if (was_valid) {
            key_slice = iter->key();
            value_slice = iter->value();
        }
    });

    store_key_t prev_key = range.left;
    key_range_t::bound_t prev_bound = key_range_t::bound_t::closed;

    // Now walk through the store.
    // TODO: What do we use memory tracker for?
    for (;;) {
        if (interruptor->is_pulsed()) {
            return continue_bool_t::ABORT;  // TODO: Is that right?
        }
        if (!was_valid) {
            break;
        }

        key_slice.remove_prefix(rocks_kv_prefix.size());

        // TODO: Batch backfill items.  (It seems nice.)

        backfill_item_t::pair_t pair;
        pair.key.assign(key_slice.size(), reinterpret_cast<const uint8_t *>(key_slice.data()));
        pair.recency = max_timestamp;  // TODO: At some point, more appropriate.
        pair.value1 = make_optional(std::vector<char>(value_slice.data(), value_slice.data() + value_slice.size()));

        backfill_item_t item;
        item.range = key_range_t(prev_bound, prev_key, key_range_t::bound_t::closed, pair.key);
        prev_key = pair.key;
        prev_bound = key_range_t::bound_t::open;
        item.min_deletion_timestamp = max_timestamp;  // TODO: A gross hack, but we can't do better for now.
        item.pairs.push_back(std::move(pair));

        if (continue_bool_t::ABORT == item_consumer->on_item(std::move(item))) {
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

    iter.reset();  // Might as well destroy asap.

    backfill_item_t item;
    item.range = key_range_t(prev_bound, prev_key.btree_key(), key_range_t::bound_t::none, nullptr);
    item.range.right = range.right;
    item.min_deletion_timestamp = max_timestamp;  // TODO: A gross hack, but we can't do better for now.

    if (item.range.is_empty()) {
        return continue_bool_t::CONTINUE;
    }

    return item_consumer->on_item(std::move(item));
}

void ignore_all_pre_items(
        auto_drainer_t::lock_t lock, key_range_t key_range,
        btree_backfill_pre_item_producer_t *pre_item_producer) {
    coro_t::spawn_later_ordered([lock, key_range, pre_item_producer]() {
        key_range_t::right_bound_t cursor(key_range.left);
        std::function<void(const backfill_pre_item_t &)> func = [](const backfill_pre_item_t &) {};
        while (cursor != key_range.right) {
            continue_bool_t cont = pre_item_producer->consume_range(&cursor, key_range.right, func);
            if (cont == continue_bool_t::ABORT) {
                return;
            }
        }
    });
}

// TODO: Rename (drop the btree prefix)
continue_bool_t btree_send_backfill(
        rockshard rocksh,
        real_superblock_t *superblock,
        release_superblock_t release_superblock,
        const key_range_t &range,
        repli_timestamp_t reference_timestamp,
        btree_backfill_pre_item_producer_t *pre_item_producer,
        btree_backfill_item_consumer_t *item_consumer,
        backfill_item_memory_tracker_t *memory_tracker,
        signal_t *interruptor) {
    // I don't know whether backfill pre-items are throttled based on incoming
    // backfill information or pre-item acks or what.  So we're going to consume
    // (and ignore) pre-items in one coroutine and send backfill items in
    // another.
    auto_drainer_t drainer;
    ignore_all_pre_items(drainer.lock(), range, pre_item_producer);

    continue_bool_t cont = send_all_in_keyrange(
        rocksh, superblock, release_superblock, range, reference_timestamp, item_consumer,
        memory_tracker, interruptor);

    drainer.drain();
    // TODO: When would we abort?
    return cont;
}

// TODO: Remove.. all these functions.

class backfill_deletion_timestamp_updater_t : public depth_first_traversal_callback_t {
public:
    backfill_deletion_timestamp_updater_t(value_sizer_t *s, repli_timestamp_t mdt) :
        sizer(s), min_deletion_timestamp(mdt) { }
    continue_bool_t handle_pre_leaf(
            const counted_t<counted_buf_lock_and_read_t> &buf,
            UNUSED const btree_key_t *left_excl_or_null,
            UNUSED const btree_key_t *right_incl,
            signal_t *,
            bool *skip_out) override {
        *skip_out = true;
        buf_write_t buf_write(&buf->lock);
        leaf_node_t *lnode = static_cast<leaf_node_t *>(buf_write.get_data_write());
        leaf::erase_deletions(sizer, lnode,
                              make_optional(min_deletion_timestamp));
        buf->lock.set_recency(superceding_recency(
            min_deletion_timestamp, buf->lock.get_recency()));
        return continue_bool_t::CONTINUE;
    }
    continue_bool_t handle_pre_internal(
            const counted_t<counted_buf_lock_and_read_t> &buf,
            UNUSED const btree_key_t *left_excl_or_null,
            UNUSED const btree_key_t *right_incl,
            signal_t *) override {
        buf->lock.set_recency(superceding_recency(
            min_deletion_timestamp, buf->lock.get_recency()));
        return continue_bool_t::CONTINUE;
    }
    continue_bool_t handle_pair(scoped_key_value_t &&, signal_t *) override {
        unreachable();
    }
private:
    value_sizer_t *sizer;
    repli_timestamp_t min_deletion_timestamp;
};

void btree_receive_backfill_item_update_deletion_timestamps(
        real_superblock_t *superblock,
        release_superblock_t release_superblock,
        value_sizer_t *sizer,
        const backfill_item_t &item,
        signal_t *interruptor) {
    backfill_debug_range(item.range, strprintf(
        "b.r.b.i.u.d.t. %" PRIu64, item.min_deletion_timestamp.longtime));
    backfill_deletion_timestamp_updater_t updater(sizer, item.min_deletion_timestamp);
    continue_bool_t res = btree_depth_first_traversal(
        superblock, item.range, &updater, access_t::write, direction_t::forward, release_superblock,
        interruptor);
    guarantee(res == continue_bool_t::CONTINUE);
}

