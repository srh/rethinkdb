#include "buffer_cache/alt.hpp"

#include <stack>

#include "arch/types.hpp"
#include "arch/runtime/coroutines.hpp"
#include "buffer_cache/stats.hpp"
#include "concurrency/auto_drainer.hpp"
#include "utils.hpp"

#define ALT_DEBUG 0

using alt::block_version_t;
using alt::current_page_acq_t;
using alt::page_acq_t;
using alt::page_cache_t;
using alt::page_t;
using alt::page_txn_t;
using alt::throttler_acq_t;

const int64_t MINIMUM_SOFT_UNWRITTEN_CHANGES_LIMIT = 1;
const int64_t SOFT_UNWRITTEN_CHANGES_LIMIT = 8000;
const double SOFT_UNWRITTEN_CHANGES_MEMORY_FRACTION = 0.5;

// In addition to the data blocks themselves, transactions that are not completely
// flushed yet consume memory for the index writes and general metadata. If
// there are a lot of soft durability transactions, these can accumulate and consume
// an increasing amount of RAM. Hence we limit the number of unwritten index
// updates in addition to the number of unwritten blocks. We scale that limit
// proportionally to the unwritten block changes limit
const int64_t INDEX_CHANGES_LIMIT_FACTOR = 5;

// There are very few ASSERT_NO_CORO_WAITING calls (instead we have
// ASSERT_FINITE_CORO_WAITING) because most of the time we're at the mercy of the
// page cache, which often may need to load or evict blocks, which may involve a
// spawn_now call.


// The intrusive list of alt_snapshot_node_t contains all the snapshot nodes for a
// given block id, in order by version. (See cache_t::snapshot_nodes_by_block_id_.)
class alt_snapshot_node_t : public intrusive_list_node_t<alt_snapshot_node_t> {
public:
    explicit alt_snapshot_node_t(scoped_ptr_t<current_page_acq_t> &&acq);
    ~alt_snapshot_node_t();

private:
    friend class cache_t;

    // This is never null (and is always a current_page_acq_t that has had
    // declare_snapshotted() called).
    scoped_ptr_t<current_page_acq_t> current_page_acq_;

    // A NULL pointer associated with a block id indicates that the block is deleted.
    std::map<block_id_t, alt_snapshot_node_t *> children_;

    // The number of buf_lock_t's referring to this node, plus the number of
    // alt_snapshot_node_t's referring to this node (via its children_ vector).
    int64_t ref_count_;


    DISABLE_COPYING(alt_snapshot_node_t);
};

alt_txn_throttler_t::alt_txn_throttler_t(int64_t minimum_unwritten_changes_limit)
    : minimum_unwritten_changes_limit_(minimum_unwritten_changes_limit),
      unwritten_block_changes_semaphore_(SOFT_UNWRITTEN_CHANGES_LIMIT),
      unwritten_index_changes_semaphore_(
          SOFT_UNWRITTEN_CHANGES_LIMIT * INDEX_CHANGES_LIMIT_FACTOR) { }

alt_txn_throttler_t::~alt_txn_throttler_t() { }

throttler_acq_t alt_txn_throttler_t::begin_txn_or_throttle(int64_t expected_change_count) {
    throttler_acq_t acq;
    acq.index_changes_semaphore_acq_.init(
        &unwritten_index_changes_semaphore_,
        expected_change_count);
    acq.index_changes_semaphore_acq_.acquisition_signal()->wait();
    acq.block_changes_semaphore_acq_.init(
        &unwritten_block_changes_semaphore_,
        expected_change_count);
    acq.block_changes_semaphore_acq_.acquisition_signal()->wait();
    return acq;
}

void alt_txn_throttler_t::end_txn(UNUSED throttler_acq_t acq) {
    // Just let the acq destructor do its thing.
}

void alt_txn_throttler_t::inform_memory_limit_change(uint64_t memory_limit,
                                                     const block_size_t max_block_size) {
    int64_t throttler_limit = std::min<int64_t>(SOFT_UNWRITTEN_CHANGES_LIMIT,
        (memory_limit / max_block_size.ser_value()) * SOFT_UNWRITTEN_CHANGES_MEMORY_FRACTION);

    // Always provide at least one capacity in the semaphore
    throttler_limit = std::max<int64_t>(throttler_limit, minimum_unwritten_changes_limit_);

    unwritten_index_changes_semaphore_.set_capacity(
        throttler_limit * INDEX_CHANGES_LIMIT_FACTOR);
    unwritten_block_changes_semaphore_.set_capacity(throttler_limit);
}

cache_t::cache_t(serializer_t *serializer,
                 cache_balancer_t *balancer,
                 perfmon_collection_t *perfmon_collection)
    : throttler_(MINIMUM_SOFT_UNWRITTEN_CHANGES_LIMIT),
      page_cache_(serializer, balancer, &throttler_),
      stats_(make_scoped<alt_cache_stats_t>(&page_cache_, perfmon_collection)) { }

cache_t::~cache_t() {
    guarantee(snapshot_nodes_by_block_id_.empty());
}

cache_account_t cache_t::create_cache_account(int priority) {
    return page_cache_.create_cache_account(priority);
}

alt_snapshot_node_t *
cache_t::matching_snapshot_node_or_null(block_id_t block_id,
                                        block_version_t block_version) {
    ASSERT_NO_CORO_WAITING;
    auto list_it = snapshot_nodes_by_block_id_.find(block_id);
    if (list_it == snapshot_nodes_by_block_id_.end()) {
        return nullptr;
    }
    intrusive_list_t<alt_snapshot_node_t> *list = &list_it->second;
    for (alt_snapshot_node_t *p = list->tail(); p != nullptr; p = list->prev(p)) {
        if (p->current_page_acq_->block_version() == block_version) {
            return p;
        }
    }
    return nullptr;
}

void cache_t::add_snapshot_node(block_id_t block_id,
                                alt_snapshot_node_t *node) {
    ASSERT_NO_CORO_WAITING;
    snapshot_nodes_by_block_id_[block_id].push_back(node);
}

void cache_t::remove_snapshot_node(block_id_t block_id, alt_snapshot_node_t *node) {
    ASSERT_FINITE_CORO_WAITING;
    // In some hypothetical cache data structure (a disk backed queue) we could have
    // a long linked list of snapshot nodes.  So we avoid _recursively_ removing
    // snapshot nodes.

    // Nodes to be deleted.
    std::stack<std::pair<block_id_t, alt_snapshot_node_t *> > stack;
    stack.push(std::make_pair(block_id, node));

    while (!stack.empty()) {
        auto pair = stack.top();
        stack.pop();
        // Step 1. Remove the node to be deleted from its list in
        // snapshot_nodes_by_block_id_.
        auto list_it = snapshot_nodes_by_block_id_.find(pair.first);
        rassert(list_it != snapshot_nodes_by_block_id_.end());
        list_it->second.remove(pair.second);
        if (list_it->second.empty()) {
            snapshot_nodes_by_block_id_.erase(list_it);
        }

        const std::map<block_id_t, alt_snapshot_node_t *> children
            = std::move(pair.second->children_);
        // Step 2. Destroy the node.
        delete pair.second;

        // Step 3. Take its children and reduce their reference count, readying them
        // for deletion if necessary.
        for (auto it = children.begin(); it != children.end(); ++it) {
#if ALT_DEBUG
            debugf("decring child %p from parent %p (in %p)\n",
                   it->second, pair.second, this);
#endif
            if (it->second != nullptr) {
                --it->second->ref_count_;
                if (it->second->ref_count_ == 0) {
#if ALT_DEBUG
                    debugf("removing child %p from parent %p (in %p)\n",
                           it->second, pair.second, this);
#endif
                    stack.push(*it);
                }
            }
        }
    }
}

txn_t::txn_t(cache_conn_t *cache_conn,
             read_access_t)
    : cache_(cache_conn->cache()),
      cache_account_(cache_->page_cache_.default_reads_account()),
      access_(access_t::read),
      durability_(write_durability_t::SOFT),
      is_committed_(false) {
    // Right now, cache_conn is only used to control flushing of write txns.  When we
    // need to support other cache_conn_t related features, we'll need to do something
    // fancier with read txns on cache conns.
    help_construct(0, nullptr);
}

txn_t::txn_t(cache_conn_t *cache_conn,
             write_durability_t durability,
             int64_t expected_change_count)
    : cache_(cache_conn->cache()),
      cache_account_(cache_->page_cache_.default_reads_account()),
      access_(access_t::write),
      durability_(durability),
      is_committed_(false) {

    help_construct(expected_change_count, cache_conn);
}

void txn_t::help_construct(int64_t expected_change_count,
                           cache_conn_t *cache_conn) {
    cache_->assert_thread();
    guarantee(expected_change_count >= 0);
    // We skip the throttler for read transactions.
    // Note that this allows read transactions to skip ahead of writes.
    if (access_ == access_t::write) {
        // To more easily detect code that assumes that transaction creation
        // does not block, we always yield in debug mode.
        DEBUG_ONLY_CODE(coro_t::yield_ordered());
    }
    throttler_acq_t throttler_acq(
        access_ == access_t::write
        ? cache_->throttler_.begin_txn_or_throttle(expected_change_count)
        : throttler_acq_t());

    ASSERT_FINITE_CORO_WAITING;

    page_txn_.init(new page_txn_t(&cache_->page_cache_,
                                  std::move(throttler_acq),
                                  cache_conn));
}

void txn_t::inform_tracker(cache_t *cache, throttler_acq_t *throttler_acq) {
    cache->throttler_.end_txn(std::move(*throttler_acq));
}

void txn_t::pulse_and_inform_tracker(cache_t *cache,
                                     throttler_acq_t *throttler_acq,
                                     cond_t *pulsee) {
    inform_tracker(cache, throttler_acq);
    pulsee->pulse();
}

txn_t::~txn_t() {
    guarantee(access_ == access_t::read || is_committed_,
        "A transaction was aborted. To avoid data corruption, we're "
        "terminating the server. Please report this bug.");

    if (access_ == access_t::read) {
        cache_->page_cache_.end_read_txn(std::move(page_txn_));
    }
}

void txn_t::commit() {
    cache_->assert_thread();

    guarantee(!is_committed_);
    guarantee(access_ == access_t::write);
    is_committed_ = true;

    if (durability_ == write_durability_t::SOFT) {
        cache_->page_cache_.flush_and_destroy_txn(std::move(page_txn_),
            std::bind(&txn_t::inform_tracker,
                cache_,
                ph::_1));
    } else {
        cond_t cond;
        cache_->page_cache_.flush_and_destroy_txn(
            std::move(page_txn_),
            std::bind(&txn_t::pulse_and_inform_tracker,
                cache_, ph::_1, &cond));
        cond.wait();
    }
}

void txn_t::set_account(cache_account_t *cache_account) {
    cache_account_ = cache_account;
}


alt_snapshot_node_t::alt_snapshot_node_t(scoped_ptr_t<current_page_acq_t> &&acq)
    : current_page_acq_(std::move(acq)), ref_count_(0) { }

alt_snapshot_node_t::~alt_snapshot_node_t() {
    // The only thing that deletes an alt_snapshot_node_t should be the
    // remove_snapshot_node function.
    rassert(ref_count_ == 0);
    rassert(current_page_acq_.has());
    rassert(children_.empty());
}
