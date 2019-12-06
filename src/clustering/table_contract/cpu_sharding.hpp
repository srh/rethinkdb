// Copyright 2010-2015 RethinkDB, all rights reserved.
#ifndef CLUSTERING_TABLE_CONTRACT_CPU_SHARDING_HPP_
#define CLUSTERING_TABLE_CONTRACT_CPU_SHARDING_HPP_

#include "protocol_api.hpp"
#include "region/region.hpp"

class branch_history_manager_t;
class store_t;

/* `multistore_ptr_t` is a bundle of `store_view_t`s, one for each CPU shard.  Actually
now there is just one shard, so this holds a store pointer and a thread allocation, and
maybe some other fluff. */

// TODO: Remove.
class multistore_ptr_t : public home_thread_mixin_t {
public:
    virtual ~multistore_ptr_t() { }

    virtual branch_history_manager_t *get_branch_history_manager() = 0;

    virtual store_view_t *get_store() = 0;

    /* The `sindex_manager_t` uses this interface to get at the underlying `store_t`s so
    it can create and destroy sindexes on them. The `table_contract` code should never
    use it, and some unit tests will return `nullptr` from here. */
    virtual store_t *get_underlying_store() = 0;
};

#endif /* CLUSTERING_TABLE_CONTRACT_CPU_SHARDING_HPP_ */

