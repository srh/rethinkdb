// Copyright 2010-2015 RethinkDB, all rights reserved.
#include "clustering/table_contract/cpu_sharding.hpp"

// TODO: Remove
region_t cpu_sharding_subspace(int subregion_number) {
    guarantee(subregion_number == THE_CPU_SHARD);

    return region_t(key_range_t::universe());
}
