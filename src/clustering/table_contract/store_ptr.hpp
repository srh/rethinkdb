// Copyright 2010-2015 RethinkDB, all rights reserved.
#ifndef CLUSTERING_TABLE_CONTRACT_STORE_PTR_HPP_
#define CLUSTERING_TABLE_CONTRACT_STORE_PTR_HPP_

#include "protocol_api.hpp"
#include "region/region.hpp"

class branch_history_manager_t;
class store_t;

/* `store_ptr_t` just holds a store, its own branch history manager, and a
thread allocation token for the store.  No more multi-stores, because there are
not CPU shards.  It could be called a store_ptr_t. */

class store_ptr_t : public home_thread_mixin_t {
public:
    virtual ~store_ptr_t() { }

    virtual branch_history_manager_t *get_branch_history_manager() = 0;

    virtual store_view_t *get_store() = 0;

    /*  Returns the same value as get_store().  mock_store_t doesn't support
    sindex creation/destruction functions, and strictly speaking those don't
    belong in store_view_t. */
    virtual store_t *get_underlying_store() = 0;
};

#endif /* CLUSTERING_TABLE_CONTRACT_STORE_PTR HPP_ */

