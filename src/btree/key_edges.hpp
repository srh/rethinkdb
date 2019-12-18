#ifndef BTREE_KEY_EDGES_HPP_
#define BTREE_KEY_EDGES_HPP_

#include "btree/keys.hpp"

// Describes a closed left bound or an open right bound.  A partition of the
// key space into "< k" and ">= k" for some k (which might be +infinity).
// Corresponds to a "lower_bound()" key boundary as used in the STL, hence
// the name lower_key_bound.  Equivalent to a key_range_t::right_bound_t.
class lower_key_bound {
public:
    bool infinite = false;
    // key is unused if infinite is true.
    store_key_t key;

    lower_key_bound() noexcept : infinite(false), key() {}
    explicit lower_key_bound(store_key_t &&k) noexcept : infinite(false), key(std::move(k)) {}

    static lower_key_bound from_right_bound(key_range_t::right_bound_t rb) {
        lower_key_bound ret;
        ret.infinite = rb.unbounded;
        ret.key = std::move(rb.internal_key);
        return ret;
    }
};

std::string key_to_debug_str(const lower_key_bound &kb);

void debug_print(printf_buffer_t *buf, const lower_key_bound &kb);
inline bool left_of_bound(const store_key_t &k, const lower_key_bound &b) {
    return b.infinite || k < b.key;
}
inline bool right_of_bound(const store_key_t &k, const lower_key_bound &b) {
    return !left_of_bound(k, b);
}

// Like key_range_t, except left can also be infinite.
class lower_key_bound_range {
public:
    lower_key_bound left;
    lower_key_bound right;

    static lower_key_bound_range from_key_range(key_range_t kr) {
        lower_key_bound_range ret;
        ret.left = lower_key_bound(std::move(kr.left));
        ret.right = lower_key_bound::from_right_bound(std::move(kr.right));
        return ret;
    }

    bool contains_key(const store_key_t &k) const {
        return right_of_bound(k, left) && left_of_bound(k, right);
    }
};

void debug_print(printf_buffer_t *buf, const lower_key_bound_range &kr);

#endif  // BTREE_KEY_EDGES_HPP_
