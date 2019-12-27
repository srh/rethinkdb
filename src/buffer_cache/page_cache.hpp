// Copyright 2010-2014 RethinkDB, all rights reserved.
#ifndef BUFFER_CACHE_PAGE_CACHE_HPP_
#define BUFFER_CACHE_PAGE_CACHE_HPP_

#include <functional>
#include <map>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

#include "concurrency/new_semaphore.hpp"

class alt_txn_throttler_t;
class cache_t;

enum class alt_create_t { create };

class cache_conn_t {
public:
    explicit cache_conn_t(cache_t *_cache)
        : cache_(_cache) { }
    ~cache_conn_t();

    cache_t *cache() const { return cache_; }
private:
    // Here for convenience, because otherwise you'd be passing around a cache_t with
    // every cache_conn_t parameter.
    cache_t *cache_;

    DISABLE_COPYING(cache_conn_t);
};


namespace alt {


class throttler_acq_t {
public:
    throttler_acq_t() { }
    ~throttler_acq_t() { }
    throttler_acq_t(throttler_acq_t &&movee)
        : block_changes_semaphore_acq_(std::move(movee.block_changes_semaphore_acq_)),
          index_changes_semaphore_acq_(std::move(movee.index_changes_semaphore_acq_)) {
        movee.block_changes_semaphore_acq_.reset();
        movee.index_changes_semaphore_acq_.reset();
    }

    // See below:  this can update how much *_changes_semaphore_acq_ holds.
    void update_dirty_page_count(int64_t new_count);

    // Sets block_changes_semaphore_acq_ to 0, but keeps index_changes_semaphore_acq_
    // as it is.
    void mark_dirty_pages_written();

private:
    friend class ::alt_txn_throttler_t;
    // At first, the number of dirty pages is 0 and *_changes_semaphore_acq_.count() >=
    // dirtied_count_.  Once the number of dirty pages gets bigger than the original
    // value of *_changes_semaphore_acq_.count(), we use
    // *_changes_semaphore_acq_.change_count() to keep the numbers equal.
    new_semaphore_in_line_t block_changes_semaphore_acq_;
    new_semaphore_in_line_t index_changes_semaphore_acq_;

    DISABLE_COPYING(throttler_acq_t);
};


}  // namespace alt


#endif  // BUFFER_CACHE_PAGE_CACHE_HPP_
