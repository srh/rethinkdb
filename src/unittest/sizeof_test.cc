// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "unittest/gtest.hpp"

#include "buffer_cache/alt.hpp"

namespace unittest {

TEST(SizeofTest, Sizes) {
    EXPECT_GT(1000u, sizeof(cache_t));
    EXPECT_GT(1000u, sizeof(txn_t));
}

}  // namespace unittest
