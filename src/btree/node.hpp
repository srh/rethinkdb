// Copyright 2010-2013 RethinkDB, all rights reserved.
#ifndef BTREE_NODE_HPP_
#define BTREE_NODE_HPP_

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <string>

#include "arch/compiler.hpp"
#include "btree/keys.hpp"
#include "buffer_cache/types.hpp"
#include "config/args.hpp"
#include "serializer/types.hpp"
#include "version.hpp"

class value_sizer_t {
public:
    value_sizer_t() { }
    virtual ~value_sizer_t() { }

    virtual int size(const void *value) const = 0;
    virtual bool fits(const void *value, int length_available) const = 0;
    virtual int max_possible_size() const = 0;
    virtual block_magic_t btree_leaf_magic() const = 0;
    virtual max_block_size_t block_size() const = 0;

private:
    DISABLE_COPYING(value_sizer_t);
};

ATTR_PACKED(struct btree_statblock_t {
    //The total number of keys in the btree
    int64_t population;

    btree_statblock_t()
        : population(0)
    { }
});
static const uint32_t BTREE_STATBLOCK_SIZE = sizeof(btree_statblock_t);


#endif // BTREE_NODE_HPP_
