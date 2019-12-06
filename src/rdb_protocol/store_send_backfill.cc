#include "rdb_protocol/store.hpp"

#include "btree/backfill.hpp"
#include "btree/reql_specific.hpp"
#include "btree/operations.hpp"
#include "clustering/immediate_consistency/version.hpp"  // For to_version_map.
#include "rdb_protocol/btree.hpp"
#include "rdb_protocol/store_metainfo.hpp"

/* After every `MAX_BACKFILL_ITEMS_PER_TXN` backfill items or backfill pre-items, we'll
release the superblock and start a new transaction. */
static const int MAX_BACKFILL_ITEMS_PER_TXN = 100;

// TODO: Under rocksdb this type might be useless.  (Right now without timestamps, it is.)
/* `limiting_btree_backfill_pre_item_consumer_t` accepts `backfill_pre_item_t`s from
`btree_send_backfill_pre()` and forwards them to the given
`store_view_t::backfill_pre_item_consumer_t`, but it aborts after it receives a certain
number of them. The purpose of this is to avoid holding the B-tree superblock for too
long. */
class limiting_btree_backfill_pre_item_consumer_t :
    public btree_backfill_pre_item_consumer_t
{
public:
    limiting_btree_backfill_pre_item_consumer_t(
            store_view_t::backfill_pre_item_consumer_t *_inner,
            key_range_t::right_bound_t *_threshold_ptr) :
        inner_aborted(false), inner(_inner), remaining(MAX_BACKFILL_ITEMS_PER_TXN),
        threshold_ptr(_threshold_ptr) { }
    continue_bool_t on_pre_item(backfill_pre_item_t &&item)
            THROWS_NOTHING {
        rassert(!inner_aborted && remaining > 0);
        --remaining;
        rassert(key_range_t::right_bound_t(item.range.left) >=
            *threshold_ptr);
        *threshold_ptr = item.range.right;
        inner_aborted =
            continue_bool_t::ABORT == inner->on_pre_item(std::move(item));
        return (inner_aborted || remaining == 0)
            ? continue_bool_t::ABORT : continue_bool_t::CONTINUE;
    }
    continue_bool_t on_empty_range(
            const key_range_t::right_bound_t &new_threshold) THROWS_NOTHING {
        rassert(!inner_aborted && remaining > 0);
        --remaining;
        rassert(new_threshold >= *threshold_ptr);
        *threshold_ptr = new_threshold;
        inner_aborted =
            continue_bool_t::ABORT == inner->on_empty_range(new_threshold);
        return (inner_aborted || remaining == 0)
            ? continue_bool_t::ABORT : continue_bool_t::CONTINUE;
    }
    bool inner_aborted;
private:
    store_view_t::backfill_pre_item_consumer_t *inner;
    size_t remaining;
    key_range_t::right_bound_t *threshold_ptr;
};

continue_bool_t store_t::send_backfill_pre(
        const region_map_t<state_timestamp_t> &start_point,
        backfill_pre_item_consumer_t *pre_item_consumer,
        signal_t *interruptor)
        THROWS_ONLY(interrupted_exc_t) {
    /* `start_point` is in the form of a `region_map_t`, so we might have different
    start timestamps for different regions. But `btree_send_backfill_pre()` expects a
    single homogeneous timestamp. So we have to do each sub-region of `start_point`
    individually. */
    std::vector<std::pair<key_range_t, repli_timestamp_t> > reference_timestamps;
    start_point.visit(
        start_point.get_domain(),
            [&](const region_t &sp_region, const state_timestamp_t &tstamp) {
            reference_timestamps.push_back(std::make_pair(
                sp_region, tstamp.to_repli_timestamp()));
        });
    /* Sort the sub-regions so we can apply them from left to right */
    std::sort(reference_timestamps.begin(), reference_timestamps.end(),
        [](const std::pair<key_range_t, repli_timestamp_t> &p1,
                const std::pair<key_range_t, repli_timestamp_t> &p2) -> bool {
            /* Note that the OS X std::sort implementation sometimes calls the
            comparison operator on an element itself. */
            guarantee(&p1 == &p2 || !p1.first.overlaps(p2.first));
            return p1.first.left < p2.first.left;
        });
    for (const auto &pair : reference_timestamps) {
        /* Within each sub-region, we may make multiple separate B-tree transactions.
        This is to avoid holding the B-tree superblock for too long at once. */
        key_range_t::right_bound_t threshold(pair.first.left);
        while (threshold != pair.first.right) {
            scoped_ptr_t<txn_t> txn;
            scoped_ptr_t<real_superblock_lock> sb;
            get_btree_superblock_and_txn_for_backfilling(
                general_cache_conn.get(), &backfill_account_, &sb, &txn);

            limiting_btree_backfill_pre_item_consumer_t
                limiter(pre_item_consumer, &threshold);

            key_range_t to_do = pair.first;
            to_do.left = threshold.key();
            continue_bool_t cont = btree_send_backfill_pre(std::move(sb),
                to_do, pair.second, &limiter, interruptor);
            guarantee(threshold <= pair.first.right);
            if (limiter.inner_aborted) {
                guarantee(cont == continue_bool_t::ABORT);
                return continue_bool_t::ABORT;
            }
            guarantee(cont == continue_bool_t::ABORT || threshold == pair.first.right);
        }
    }
    return continue_bool_t::CONTINUE;
}

/* `pre_item_adapter_t` converts a `store_view_t::backfill_pre_item_producer_t` into a
`btree_backfill_pre_item_producer_t`. */
class pre_item_adapter_t : public btree_backfill_pre_item_producer_t {
public:
    explicit pre_item_adapter_t(store_view_t::backfill_pre_item_producer_t *_inner) :
        aborted(false), inner(_inner) { }
    continue_bool_t consume_range(
            key_range_t::right_bound_t *cursor_inout,
            const key_range_t::right_bound_t &limit,
            const std::function<void(const backfill_pre_item_t &)> &callback) {
        guarantee(!aborted);
        if (continue_bool_t::ABORT ==
                inner->consume_range(cursor_inout, limit, callback)) {
            aborted = true;
            return continue_bool_t::ABORT;
        } else {
            return continue_bool_t::CONTINUE;
        }
    }
    bool try_consume_empty_range(const key_range_t &range) {
        guarantee(!aborted);
        return inner->try_consume_empty_range(range);
    }
    bool aborted;
private:
    store_view_t::backfill_pre_item_producer_t *inner;
};

/* `limiting_btree_backfill_item_consumer_t` is like the `..._pre_item_consumer_t` type
defined earlier in this file, except for items instead of pre-items. It also takes care
of handling metainfo. */
class limiting_btree_backfill_item_consumer_t :
    public btree_backfill_item_consumer_t {
public:
    limiting_btree_backfill_item_consumer_t(
            store_view_t::backfill_item_consumer_t *_inner,
            key_range_t::right_bound_t *_threshold_ptr,
            const region_map_t<version_t> *_metainfo_ptr) :
        remaining(MAX_BACKFILL_ITEMS_PER_TXN), inner(_inner),
        threshold_ptr(_threshold_ptr), metainfo_ptr(_metainfo_ptr) { }
    continue_bool_t on_item(backfill_item_t &&item) override {
        rassert(remaining > 0);
        --remaining;
        rassert(key_range_t::right_bound_t(item.range.left) >=
            *threshold_ptr,
            "compare bound '%s' to '%s'", key_to_unescaped_str(item.range.left).c_str(),
            threshold_ptr->unbounded ? "(unbounded)" : key_to_unescaped_str(threshold_ptr->key()).c_str()
            );
        *threshold_ptr = item.range.right;
        inner->on_item(*metainfo_ptr, std::move(item));
        return remaining == 0
            ? continue_bool_t::ABORT : continue_bool_t::CONTINUE;
    }
    continue_bool_t on_empty_range(
            const key_range_t::right_bound_t &new_threshold) override {
        rassert(remaining > 0);
        --remaining;
        rassert(new_threshold >= *threshold_ptr);
        *threshold_ptr = new_threshold;
        inner->on_empty_range(*metainfo_ptr, new_threshold);
        return remaining == 0
            ? continue_bool_t::ABORT : continue_bool_t::CONTINUE;
    }
    size_t remaining;
private:
    store_view_t::backfill_item_consumer_t *const inner;
    key_range_t::right_bound_t *const threshold_ptr;

    /* `metainfo_ptr` points to the metainfo that applies to the items we're handling.
    Note that it can't be changed. This is OK because `limiting_..._consumer_t` never
    exists across multiple B-tree transactions, so the metainfo is constant. */
    const region_map_t<version_t> *const metainfo_ptr;
};

continue_bool_t store_t::send_backfill(
        const region_map_t<state_timestamp_t> &start_point,
        backfill_pre_item_producer_t *pre_item_producer,
        store_view_t::backfill_item_consumer_t *item_consumer,
        backfill_item_memory_tracker_t *memory_tracker,
        signal_t *interruptor)
        THROWS_ONLY(interrupted_exc_t) {
    /* Just like in `send_backfill_pre()`, we first break `start_point` up into regions
    with homogeneous start timestamps, then backfill each region as a series of multiple
    B-tree transactions to avoid holding the superblock too long. */
    std::vector<std::pair<key_range_t, repli_timestamp_t> > reference_timestamps;
    start_point.visit(
        start_point.get_domain(),
        [&](const region_t &sp_region, const state_timestamp_t &tstamp) {
            reference_timestamps.push_back(std::make_pair(
                sp_region, tstamp.to_repli_timestamp()));
        });
    std::sort(reference_timestamps.begin(), reference_timestamps.end(),
        [](const std::pair<key_range_t, repli_timestamp_t> &p1,
                const std::pair<key_range_t, repli_timestamp_t> &p2) -> bool {
            /* Note that the OS X std::sort implementation sometimes calls the
            comparison operator on an element itself. */
            guarantee(&p1 == &p2 || !p1.first.overlaps(p2.first));
            return p1.first.left < p2.first.left;
        });
    for (const auto &pair : reference_timestamps) {
        key_range_t::right_bound_t threshold(pair.first.left);
        while (threshold != pair.first.right) {
            scoped_ptr_t<txn_t> txn;
            scoped_ptr_t<real_superblock_lock> sb;
            get_btree_superblock_and_txn_for_backfilling(
                general_cache_conn.get(), &backfill_account_, &sb, &txn);

            pre_item_producer->rewind(threshold);
            pre_item_adapter_t pre_item_adapter(pre_item_producer);

            region_map_t<version_t> metainfo_copy =
                metainfo->get(sb.get(), region_t(pair.first));

            key_range_t to_do = pair.first;
            to_do.left = threshold.key();

            // TODO: This knowledge of what the metainfo contains is technically above the store's pay-grade, right?
            // (And this is a hack to talk backfilling on the old cluster protocol, half-decently.)
            repli_timestamp_t max_timestamp = repli_timestamp_t::distant_past;

            region_t to_do_region(to_do);
            metainfo_copy.visit(to_do_region, [&](const region_t &, const version_t version) {
                max_timestamp = std::max<repli_timestamp_t>(max_timestamp, version.timestamp.to_repli_timestamp());
            });

            limiting_btree_backfill_item_consumer_t limiter(
                item_consumer, &threshold, &metainfo_copy);

            continue_bool_t cont = btree_send_backfill(
                rocksh(), std::move(sb), to_do, pair.second, max_timestamp,
                &pre_item_adapter, &limiter, memory_tracker, interruptor);

            /* Check if the backfill was aborted because of exhausting the memory
            limit, or because the pre_item_adapter aborted.
            Note that `memory_tracker->is_limit_exceeded()` can sometimes return
            `true` even though that wasn't the reason for the backfill being aborted.
            In particular this can happen if `memory_tracker->note_item()` was called
            after the last time that `memory_tracker->is_limit_exceeded()` was checked
            in the backfill. The next loop iteration would abort anyway because of
            the exceeded limit, so aborting now even if that wasn't the reason for
            `cont` being set to `continue_bool_t::ABORT` isn't a big deal. */
            if ((cont == continue_bool_t::ABORT && memory_tracker->is_limit_exceeded())
                || pre_item_adapter.aborted) {
                guarantee(cont == continue_bool_t::ABORT);
                return continue_bool_t::ABORT;
            }
            guarantee((limiter.remaining == 0) == (cont == continue_bool_t::ABORT));
            guarantee(threshold == pair.first.right || cont == continue_bool_t::ABORT);
        }
    }
    return continue_bool_t::CONTINUE;
}

