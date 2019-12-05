// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "region/region.hpp"

#include <algorithm>

void debug_print(printf_buffer_t *buf, const hash_range_t &hr) {
    buf->appendf("hash_range_t(%" PRIx64 ", %" PRIx64 ")", hr.beg, hr.end);
}

bool compare_range_by_left(const key_range_t &r1, const key_range_t &r2) {
    return r1.left < r2.left;
}

region_join_result_t region_join(const std::vector<key_range_t> &vec, key_range_t *out) THROWS_NOTHING {
    if (vec.empty()) {
        *out = key_range_t::empty();
        return REGION_JOIN_OK;
    } else {
        std::vector<key_range_t> sorted = vec;
        std::sort(sorted.begin(), sorted.end(), &compare_range_by_left);
        key_range_t::right_bound_t cursor = key_range_t::right_bound_t(sorted[0].left);
        for (size_t i = 0; i < sorted.size(); ++i) {
            if (cursor < key_range_t::right_bound_t(sorted[i].left)) {
                return REGION_JOIN_BAD_REGION;
            } else if (cursor > key_range_t::right_bound_t(sorted[i].left)) {
                return REGION_JOIN_BAD_JOIN;
            } else {
                /* The regions match exactly; move on to the next one. */
                cursor = sorted[i].right;
            }
        }
        key_range_t key_union;
        key_union.left = sorted[0].left;
        key_union.right = cursor;
        *out = key_union;
        return REGION_JOIN_OK;
    }
}

