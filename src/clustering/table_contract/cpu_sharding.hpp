// Copyright 2010-2015 RethinkDB, all rights reserved.
#ifndef CLUSTERING_TABLE_CONTRACT_CPU_SHARDING_HPP_
#define CLUSTERING_TABLE_CONTRACT_CPU_SHARDING_HPP_

#include "protocol_api.hpp"
#include "region/region.hpp"

class branch_history_manager_t;
class store_t;

/* `cpu_sharding_subspace()` returns a `region_t` that contains the full key-range space
but only 1/CPU_SHARDING_FACTOR of the shard space. */
region_t cpu_sharding_subspace(int subregion_number);


/* `get_cpu_shard_approx_number()` is like `get_cpu_shard_number()`, except that if the
input doesn't correspond exactly to a CPU shard, it returns an estimate. */
int get_cpu_shard_approx_number(const region_t &region);

/* `multistore_ptr_t` is a bundle of `store_view_t`s, one for each CPU shard. The rule
is that `get_cpu_sharded_store(i)->get_region() == cpu_sharding_subspace(i)`. The
individual stores' home threads may be different from the `multistore_ptr_t`'s home
thread. */

// TODO: Remove.
class multistore_ptr_t : public home_thread_mixin_t {
public:
    virtual ~multistore_ptr_t() { }

    virtual branch_history_manager_t *get_branch_history_manager() = 0;

    // TODO: Remove
    virtual store_view_t *get_cpu_sharded_store(size_t i) = 0;
    virtual store_view_t *get_store() = 0;

    /* The `sindex_manager_t` uses this interface to get at the underlying `store_t`s so
    it can create and destroy sindexes on them. The `table_contract` code should never
    use it, and some unit tests will return `nullptr` from here. */
    // TODO: Remove
    virtual store_t *get_underlying_store(size_t i) = 0;
    virtual store_t *get_underlying_store() = 0;
};

#endif /* CLUSTERING_TABLE_CONTRACT_CPU_SHARDING_HPP_ */

