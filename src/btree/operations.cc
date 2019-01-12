// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "btree/operations.hpp"

#include <stdint.h>

#include "btree/internal_node.hpp"
#include "btree/leaf_node.hpp"
#include "buffer_cache/alt.hpp"
#include "buffer_cache/blob.hpp"
#include "containers/archive/vector_stream.hpp"
#include "rdb_protocol/profile.hpp"
#include "rdb_protocol/store.hpp"

block_id_t create_stat_block(buf_parent_t parent) {
    buf_lock_t stats_block(parent, alt_create_t::create);
    buf_write_t write(&stats_block);
    // Make the stat block be the default constructed stats block.
    *static_cast<btree_statblock_t *>(write.get_data_write(BTREE_STATBLOCK_SIZE))
        = btree_statblock_t();
    return stats_block.block_id();
}

