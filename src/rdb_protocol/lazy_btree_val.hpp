// Copyright 2010-2015 RethinkDB, all rights reserved.
#ifndef RDB_PROTOCOL_LAZY_BTREE_VAL_HPP_
#define RDB_PROTOCOL_LAZY_BTREE_VAL_HPP_

#include "buffer_cache/alt.hpp"
#include "buffer_cache/blob.hpp"
#include "rdb_protocol/datum.hpp"

struct rdb_value_t {
    char contents[1];

public:
    int inline_size(max_block_size_t bs) const {
        return blob::ref_size(bs, contents, blob::btree_maxreflen);
    }

    int64_t value_size() const {
        return blob::value_size(contents, blob::btree_maxreflen);
    }

    const char *value_ref() const {
        return contents;
    }

    char *value_ref() {
        return contents;
    }
};



#endif  // RDB_PROTOCOL_LAZY_BTREE_VAL_HPP_
