// Copyright 2010-2014 RethinkDB, all rights reserved.
#ifndef BUFFER_CACHE_ALT_HPP_
#define BUFFER_CACHE_ALT_HPP_

#include <map>
#include <vector>
#include <utility>
#include <unordered_map>

#include "buffer_cache/page_cache.hpp"
#include "buffer_cache/types.hpp"
#include "concurrency/rwlock.hpp"
#include "containers/two_level_array.hpp"
#include "containers/uuid.hpp"
#include "repli_timestamp.hpp"

class serializer_t;

class alt_cache_stats_t;
class alt_snapshot_node_t;
class perfmon_collection_t;
class cache_balancer_t;

class alt_txn_throttler_t {
public:
    explicit alt_txn_throttler_t(int64_t minimum_unwritten_changes_limit);
    ~alt_txn_throttler_t();

    alt::throttler_acq_t begin_txn_or_throttle(int64_t expected_change_count);
    void end_txn(alt::throttler_acq_t acq);

    void inform_memory_limit_change(uint64_t memory_limit,
                                    block_size_t max_block_size);

private:
    const int64_t minimum_unwritten_changes_limit_;

    new_semaphore_t unwritten_block_changes_semaphore_;
    new_semaphore_t unwritten_index_changes_semaphore_;

    DISABLE_COPYING(alt_txn_throttler_t);
};

class lock_state {
public:
    rwlock_t real_superblock_lock;
    rwlock_t sindex_block_lock;
    std::unordered_map<uuid_u, scoped_ptr_t<rwlock_t>> sindex_superblock_locks;
};

class cache_t : public home_thread_mixin_t {
public:
    explicit cache_t(serializer_t *serializer,
                     cache_balancer_t *balancer,
                     perfmon_collection_t *perfmon_collection);
    ~cache_t();

    max_block_size_t max_block_size() const { return page_cache_.max_block_size(); }

    // These todos come from the mirrored cache.  The real problem is that whole
    // cache account / priority thing is just one ghetto hack amidst a dozen other
    // throttling systems.  TODO: Come up with a consistent priority scheme,
    // i.e. define a "default" priority etc.  TODO: As soon as we can support it, we
    // might consider supporting a mem_cap paremeter.
    cache_account_t create_cache_account(int priority);

private:
    friend class txn_t;
    friend class real_superblock_lock;
    friend class sindex_block_lock;
    friend class sindex_superblock_lock;

    alt_snapshot_node_t *matching_snapshot_node_or_null(
            block_id_t block_id,
            alt::block_version_t block_version);
    void add_snapshot_node(block_id_t block_id, alt_snapshot_node_t *node);
    void remove_snapshot_node(block_id_t block_id, alt_snapshot_node_t *node);

    // throttler_ can cause the txn_t constructor to block
    alt_txn_throttler_t throttler_;

    lock_state locks_;


    alt::page_cache_t page_cache_;

    scoped_ptr_t<alt_cache_stats_t> stats_;

    std::map<block_id_t, intrusive_list_t<alt_snapshot_node_t> >
        snapshot_nodes_by_block_id_;

    DISABLE_COPYING(cache_t);
};

class txn_t {
public:
    // Constructor for read-only transactions.
    txn_t(cache_conn_t *cache_conn, read_access_t read_access);

    txn_t(cache_conn_t *cache_conn,
          write_durability_t durability,
          int64_t expected_change_count);

    ~txn_t();

    // Every write transaction must be committed before it's
    // destructed.
    // There is no roll-back / abort! Destructing an uncommitted
    // write-transaction will terminate the server.
    void commit();

    cache_t *cache() { return cache_; }
    alt::page_txn_t *page_txn() { return page_txn_.get(); }
    access_t access() const { return access_; }

    void set_account(cache_account_t *cache_account);
    cache_account_t *account() { return cache_account_; }

private:
    // Resets the *throttler_acq parameter.
    static void inform_tracker(cache_t *cache,
                               alt::throttler_acq_t *throttler_acq);

    // Resets the *throttler_acq parameter.
    static void pulse_and_inform_tracker(cache_t *cache,
                                         alt::throttler_acq_t *throttler_acq,
                                         cond_t *pulsee);


    void help_construct(int64_t expected_change_count, cache_conn_t *cache_conn);

    cache_t *const cache_;

    // Initialized to cache()->page_cache_.default_cache_account(), and modified by
    // set_account().
    cache_account_t *cache_account_;

    const access_t access_;

    // Only applicable if access_ == write.
    const write_durability_t durability_;

    scoped_ptr_t<alt::page_txn_t> page_txn_;

    bool is_committed_;

    DISABLE_COPYING(txn_t);
};

// TODO: Public/private properly.
// TODO: Move impls to .cc file.

inline void wait_for_rwlock(rwlock_in_line_t *acq, access_t access) {
    if (access == access_t::read) {
        acq->read_signal()->wait_lazily_ordered();
    } else {
        acq->write_signal()->wait_lazily_ordered();
    }
}

class real_superblock_lock {
public:
    explicit real_superblock_lock(txn_t *txn, access_t access)
        : txn_(txn), acq_(&txn->cache()->locks_.real_superblock_lock, access) {}

    txn_t *txn() { return txn_; }

    const signal_t *read_acq_signal() { return acq_.read_signal(); }
    const signal_t *write_acq_signal() { return acq_.write_signal(); }

    void reset_buf_lock() {
        txn_ = nullptr;
        acq_.reset();
    }

    txn_t *txn_;
    rwlock_in_line_t acq_;
};
class sindex_block_lock {
public:
    sindex_block_lock() : txn_(nullptr), access_(valgrind_undefined(access_t::read)), acq_() {}
    sindex_block_lock(real_superblock_lock *parent, access_t access)
        : txn_(parent->txn()), access_(access), acq_() {
        wait_for_rwlock(&parent->acq_, access);
        acq_ = make_scoped<rwlock_in_line_t>(&txn_->cache()->locks_.sindex_block_lock, access);
    }

    sindex_block_lock(sindex_block_lock &&other)
            : txn_(other.txn_), access_(other.access_), acq_(std::move(other.acq_)) {
        other.txn_ = nullptr;
        other.access_ = valgrind_undefined(access_t::read);
    }

    sindex_block_lock &operator=(sindex_block_lock&& other) {
        sindex_block_lock tmp(std::move(other));
        std::swap(txn_, tmp.txn_);
        std::swap(access_, tmp.access_);
        std::swap(acq_, tmp.acq_);
        return *this;
    }

    bool empty() const { return txn_ == nullptr; }
    access_t access() const { return access_; }

    txn_t *txn() { return txn_; }
    const signal_t *read_acq_signal() { return acq_->read_signal(); }
    const signal_t *write_acq_signal() { return acq_->write_signal(); }

    void reset_buf_lock() {
        txn_ = nullptr;
        acq_.reset();
    }

    txn_t *txn_;
    // I think only used for assertions.
    access_t access_;
    // Just to make move operator easy to implement here, in a scoped.
    scoped_ptr_t<rwlock_in_line_t> acq_;
};
class sindex_superblock_lock {
public:
    sindex_superblock_lock(sindex_block_lock *parent, uuid_u sindex_uuid, access_t access)
        : txn_(parent->txn()), acq_() {
        wait_for_rwlock(parent->acq_.get(), access);
        auto it = txn_->cache()->locks_.sindex_superblock_locks.find(sindex_uuid);
        // TODO: Precisely manage lock construction, building the sindex_superblock_locks at initial
        // table load?
        // TODO: Someplace, we need to delete locks.
        if (it == txn_->cache()->locks_.sindex_superblock_locks.end()) {
            auto res = txn_->cache()->locks_.sindex_superblock_locks.emplace(
                sindex_uuid,
                make_scoped<rwlock_t>());
            it = res.first;
        }
        acq_.init(it->second.get(), access);
    }

    sindex_superblock_lock(sindex_block_lock *parent, uuid_u sindex_uuid, alt_create_t)
        : txn_(parent->txn()), acq_() {
        wait_for_rwlock(parent->acq_.get(), access_t::write);
        auto it = txn_->cache()->locks_.sindex_superblock_locks.find(sindex_uuid);
        guarantee(it == txn_->cache()->locks_.sindex_superblock_locks.end());
        scoped_ptr_t<rwlock_t> the_lock = make_scoped<rwlock_t>();
        acq_.init(the_lock.get(), access_t::write);
        txn_->cache()->locks_.sindex_superblock_locks.emplace(
            sindex_uuid, std::move(the_lock));
    }

    const signal_t *read_acq_signal() { return acq_.read_signal(); }
    const signal_t *write_acq_signal() { return acq_.write_signal(); }

    void reset_buf_lock() {
        txn_ = nullptr;
        acq_.reset();
    }

    block_id_t block_id() const { return block_id_t(1); }  // TODO: Remove this ghetto hack.

    void mark_deleted() {
        // TODO: Implement for real (remove from sindex_superblock_locks -- force caller to hold parent lock, see if that's reasonable)
    }

    txn_t *txn_;
    rwlock_in_line_t acq_;
};



#endif  // BUFFER_CACHE_ALT_HPP_
