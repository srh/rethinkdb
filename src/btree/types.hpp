// Copyright 2010-2014 RethinkDB, all rights reserved.
#ifndef BTREE_TYPES_HPP_
#define BTREE_TYPES_HPP_

#include "errors.hpp"

enum class continue_bool_t { CONTINUE = 0, ABORT = 1 };

enum class release_superblock_t {RELEASE, KEEP};

enum class is_stamp_read_t { NO, YES };

#endif /* BTREE_TYPES_HPP_ */
