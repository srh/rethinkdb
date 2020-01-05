#include "buffer_cache/alt.hpp"

#include <stack>

#include "arch/types.hpp"
#include "arch/runtime/coroutines.hpp"
#include "buffer_cache/block_version.hpp"
#include "buffer_cache/cache_account.hpp"
#include "buffer_cache/page_cache.hpp"
#include "concurrency/auto_drainer.hpp"
#include "rockstore/store.hpp"
#include "utils.hpp"

#define ALT_DEBUG 0

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


void alt_txn_throttler_t::inform_memory_limit_change(uint64_t memory_limit) {
    // TODO: Hard-coded 4096 for removed max_block_size parameter -- this is kind of B.S.
    int64_t throttler_limit = std::min<int64_t>(SOFT_UNWRITTEN_CHANGES_LIMIT,
        (memory_limit / 4096) * SOFT_UNWRITTEN_CHANGES_MEMORY_FRACTION);

    // Always provide at least one capacity in the semaphore
    throttler_limit = std::max<int64_t>(throttler_limit, minimum_unwritten_changes_limit_);

    unwritten_index_changes_semaphore_.set_capacity(
        throttler_limit * INDEX_CHANGES_LIMIT_FACTOR);
    unwritten_block_changes_semaphore_.set_capacity(throttler_limit);
}

cache_t::cache_t(perfmon_collection_t *perfmon_collection)
    : store(),
      throttler_(MINIMUM_SOFT_UNWRITTEN_CHANGES_LIMIT) {
    (void)perfmon_collection;
    // TODO: Use perfmon_collection for something?
}

cache_t::~cache_t() {
}

cache_account_t cache_t::create_cache_account(int priority) {
    // TODO: What to do here?
    (void)priority;
    return cache_account_t();
}

// TODO: Find out what to do with the default reads account.
scoped_ptr_t<cache_account_t> default_reads_account = make_scoped<cache_account_t>();
cache_account_t *get_default_reads_account() { return default_reads_account.get(); }

txn_t::txn_t(cache_conn_t *cache_conn,
             read_access_t)
    : cache_(cache_conn->cache()),
      cache_account_(get_default_reads_account()),
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
      cache_account_(get_default_reads_account()),
      access_(access_t::write),
      durability_(durability),
      is_committed_(false) {

    help_construct(expected_change_count, cache_conn);
}

void txn_t::help_construct(int64_t expected_change_count,
                           cache_conn_t *cache_conn) {
    (void)cache_conn;  // TODO
    cache_->assert_thread();
    guarantee(expected_change_count >= 0);
    // We skip the throttler for read transactions.
    // Note that this allows read transactions to skip ahead of writes.
    if (access_ == access_t::write) {
        // To more easily detect code that assumes that transaction creation
        // does not block, we always yield in debug mode.
        DEBUG_ONLY_CODE(coro_t::yield_ordered());
    }

    throttler_acq_ = make_scoped<throttler_acq_t>(
        access_ == access_t::write
        ? cache_->throttler_.begin_txn_or_throttle(expected_change_count)
        : throttler_acq_t());
}

txn_t::~txn_t() {
    // TODO: Yeah, remove this guarantee or have txn_t::commit actually do something.
    guarantee(access_ == access_t::read || is_committed_,
        "A transaction was aborted. To avoid data corruption, we're "
        "terminating the server. Please report this bug.");
}

void txn_t::commit(rockstore::store *rocks, scoped_ptr_t<real_superblock_lock> superblock) {
    cache_->assert_thread();

    guarantee(!is_committed_);
    guarantee(access_ == access_t::write);
    is_committed_ = true;

    bool sync = durability_ == write_durability_t::SOFT ? false : true;

    rocks->write_batch(superblock->wait_write_batch()->GetWriteBatch(), rockstore::write_options(sync));
    cache_->store.batch = make_scoped<rocksdb::WriteBatchWithIndex>();
    guarantee(cache_->store.high_waterline == superblock->write_acq_sequence_number_ - 1);
    cache_->store.high_waterline = superblock->write_acq_sequence_number_;
    superblock.reset();
}

void txn_t::set_account(cache_account_t *cache_account) {
    cache_account_ = cache_account;
}
