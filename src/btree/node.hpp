// Copyright 2010-2013 RethinkDB, all rights reserved.
#ifndef BTREE_NODE_HPP_
#define BTREE_NODE_HPP_

#include <stdint.h>

#include "arch/compiler.hpp"

ATTR_PACKED(struct btree_statblock_t {
    //The total number of keys in the btree
    int64_t population;

    btree_statblock_t()
        : population(0)
    { }
});
static const uint32_t BTREE_STATBLOCK_SIZE = sizeof(btree_statblock_t);


#endif // BTREE_NODE_HPP_
