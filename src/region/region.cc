// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "region/region.hpp"

#include <algorithm>

bool compare_range_by_left(const key_range_t &r1, const key_range_t &r2) {
    return r1.left < r2.left;
}

MUST_USE region_join_result_t region_join(const std::vector<key_range_t> &vec,
                                          key_range_t *out) {
    if (vec.empty()) {
        *out = key_range_t::empty();
        return REGION_JOIN_OK;
    }
    // indices gets sorted by vec[_].left.
    std::vector<size_t> indices;
    indices.reserve(vec.size());
    for (size_t i = 0, e = vec.size(); i < e; ++i) {
        indices.push_back(i);
    }

    std::sort(indices.begin(), indices.end(), [&vec](size_t i, size_t j) {
        return vec[i].left < vec[j].left;
    });

    bool gap = false;
    for (size_t i = 1, e = indices.size(); i < e; ++i) {
        const key_range_t::right_bound_t &rb = vec[indices[i-1]].right;
        const store_key_t &key = vec[indices[i]].left;
        // We're careful to check before any overlap _before_
        // returning REGION_JOIN_BAD_REGION.  Simply because that
        // is how the hash_region version of this function behaved.
        if (rb.unbounded || rb.key() > key) {
            return REGION_JOIN_BAD_JOIN;
        }
        if (rb.key() < key) {
            gap = true;
        }
    }

    if (gap) {
        return REGION_JOIN_BAD_REGION;
    }

    out->left = vec[indices[0]].left;
    out->right = vec[indices[indices.size()-1]].right;
    return REGION_JOIN_OK;
}
