// Copyright 2010-2014 RethinkDB, all rights reserved.
#ifndef BTREE_GET_DISTRIBUTION_HPP_
#define BTREE_GET_DISTRIBUTION_HPP_

#include <vector>

#include "btree/keys.hpp"
#include "buffer_cache/types.hpp"

class rockshard;
class real_superblock_lock;

void get_distribution(
    rockshard rocksh, key_range_t key_range, int keys_limit,
    std::vector<store_key_t> *keys_out, std::vector<uint64_t> *counts_out);


#endif /* BTREE_GET_DISTRIBUTION_HPP_ */
