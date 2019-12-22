#ifndef BTREE_KEY_EDGES_HPP_
#define BTREE_KEY_EDGES_HPP_

#include "btree/keys.hpp"

// Describes a closed left bound or an open right bound.  A partition of the key
// space into "< k" and ">= k" for some k (which might be +infinity).
// Corresponds to a "lower_bound()" key boundary as used in the STL, hence the
// name lower_key_bound.  Equivalent to a key_range_t::right_bound_t.
//
// This and lower_key_bound_range were added in place of some uses of the max
// store key value "\xFF\xFF...", which were malperformant and fragile.  See
// also key_or_max.
class lower_key_bound {
public:
    bool infinite = false;
    // key is unused if infinite is true.
    store_key_t key;

    lower_key_bound() noexcept : infinite(false), key() {}
    explicit lower_key_bound(store_key_t &&k) noexcept : infinite(false), key(std::move(k)) {}
    explicit lower_key_bound(const store_key_t &k) noexcept : infinite(false), key(k) {}

    static lower_key_bound from_right_bound(key_range_t::right_bound_t rb) {
        lower_key_bound ret;
        ret.infinite = rb.unbounded;
        ret.key = std::move(rb.internal_key);
        return ret;
    }

    static lower_key_bound infinity() {
        lower_key_bound ret;
        ret.infinite = true;
        return ret;
    }
    static lower_key_bound min() {
        return lower_key_bound();
    }

    bool operator<(const lower_key_bound &rhs) const {
        return infinite ? false : rhs.infinite ? true : key < rhs.key;
    }
    bool operator>(const lower_key_bound &rhs) const {
        return rhs < *this;
    }

    bool operator==(const lower_key_bound &rhs) const {
        return infinite ? rhs.infinite : key == rhs.key;
    }
};

RDB_DECLARE_SERIALIZABLE_FOR_CLUSTER(lower_key_bound);

static const lower_key_bound lower_key_bound_min = lower_key_bound::min();
static const lower_key_bound lower_key_bound_infinity = lower_key_bound::infinity();

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

    static lower_key_bound_range half_open(lower_key_bound left, lower_key_bound right) {
        lower_key_bound_range ret;
        ret.left = std::move(left);
        ret.right = std::move(right);
        return ret;
    }

    bool contains_key(const store_key_t &k) const {
        return right_of_bound(k, left) && left_of_bound(k, right);
    }

    bool is_empty() const {
        return !(left < right);
    }
};

key_range_t::right_bound_t to_right_bound(lower_key_bound kb);
key_range_t half_open_key_range(store_key_t left, lower_key_bound right);

// May convert empty key ranges to any empty key range.
key_range_t to_key_range(lower_key_bound_range kr);

void debug_print(printf_buffer_t *buf, const lower_key_bound_range &kr);

#endif  // BTREE_KEY_EDGES_HPP_
