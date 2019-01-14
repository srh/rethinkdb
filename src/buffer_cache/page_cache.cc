// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "buffer_cache/page_cache.hpp"

#include <algorithm>
#include <functional>
#include <iterator>
#include <stack>

#include "arch/runtime/coroutines.hpp"
#include "arch/runtime/runtime.hpp"
#include "arch/runtime/runtime_utils.hpp"
#include "concurrency/auto_drainer.hpp"
#include "concurrency/new_mutex.hpp"
#include "do_on_thread.hpp"
#include "stl_utils.hpp"

cache_conn_t::~cache_conn_t() {
}


namespace alt {

// TODO: This is nonsense -- we don't call this, right?  And there are no pages.
void throttler_acq_t::update_dirty_page_count(int64_t new_count) {
    rassert(
        block_changes_semaphore_acq_.count() == index_changes_semaphore_acq_.count());
    if (new_count > block_changes_semaphore_acq_.count()) {
        block_changes_semaphore_acq_.change_count(new_count);
        index_changes_semaphore_acq_.change_count(new_count);
    }
}

void throttler_acq_t::mark_dirty_pages_written() {
    block_changes_semaphore_acq_.change_count(0);
}




}  // namespace alt
