#ifndef BTREE_KEY_EDGES_HPP_
#define BTREE_KEY_EDGES_HPP_

#include "btree/keys.hpp"

// Key or max extends the keyspace with the value +infinity.  This is used in
// place of code which previously used "\xFF\xFF..." in places.  The purpose of
// this type is (or was) to replace such uses of store_key_t and the max key
// value in a type safe manner.  In some places it might be used like a
// key_range_t::right_bound_t, representing a partition of the keyspace into
// two.
class key_or_max {
public:
    bool infinite = false;
    store_key_t key;

    key_or_max() noexcept : infinite(false), key() {}
    explicit key_or_max(store_key_t &&k) noexcept : infinite(false), key(std::move(k)) {}
    explicit key_or_max(const store_key_t &k) noexcept : infinite(false), key(k) {}

    static key_or_max infinity() {
        key_or_max ret;
        ret.infinite = true;
        return ret;
    }
    static key_or_max min() {
        return key_or_max();
    }

    bool is_min() const {
        return !infinite && key.size() == 0;
    }

    bool operator<(const key_or_max &rhs) const {
        return infinite ? false : rhs.infinite ? true : key < rhs.key;
    }
    bool operator<=(const key_or_max &rhs) const {
        return rhs.infinite || (!infinite && key <= rhs.key);
    }

    bool less_than_key(const store_key_t &rhs) const {
        return !infinite && key < rhs;
    }

    bool operator>(const key_or_max &rhs) const { return !(operator<=(rhs)); }
    bool lequal_to_key(const store_key_t &rhs) const {
        return !infinite && key <= rhs;
    }
    bool greater_than_key(const store_key_t &rhs) const {
        return !lequal_to_key(rhs);
    }

    bool operator==(const key_or_max &rhs) const {
        return infinite ? rhs.infinite : key == rhs.key;
    }

    static key_or_max from_right_bound(const key_range_t::right_bound_t &rb) {
        key_or_max ret;
        ret.infinite = rb.unbounded;
        ret.key = rb.internal_key;
        return ret;
    }

    key_range_t::right_bound_t to_right_bound() && {
        key_range_t::right_bound_t ret;
        ret.unbounded = infinite;
        ret.internal_key = std::move(key);
        return ret;
    }
};

RDB_DECLARE_SERIALIZABLE_FOR_CLUSTER(key_or_max);

void debug_print(printf_buffer_t *buf, const key_or_max &km);

// Describes a closed left bound or an open right bound.  A partition of the key
// space into "< k" and ">= k" for some k (which might be +infinity).
// Corresponds to a "lower_bound()" key boundary as used in the STL, hence the
// name lower_key_bound.  Equivalent to a key_range_t::right_bound_t.  This type
// name _might_ be misused in some places where key_or_max is more appropriate,
// particularly when iterating in a particular direction (best_unpopped_key?)
// because that's how a behavior-preserving refactoring rolled out.
using lower_key_bound = key_or_max;

std::string key_to_debug_str(const key_or_max &kb);

inline bool left_of_bound(const store_key_t &k, const lower_key_bound &b) {
    return b.infinite || k < b.key;
}
inline bool right_of_bound(const store_key_t &k, const lower_key_bound &b) {
    return !left_of_bound(k, b);
}

// Like key_range_t, except left can also be infinite.
class lower_key_bound_range {
public:
    key_or_max left;
    key_or_max right;

    static lower_key_bound_range from_key_range(key_range_t kr) {
        lower_key_bound_range ret;
        ret.left = key_or_max(std::move(kr.left));
        ret.right = key_or_max::from_right_bound(std::move(kr.right));
        return ret;
    }

    static lower_key_bound_range half_open(key_or_max left, key_or_max right) {
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

key_range_t half_open_key_range(store_key_t left, key_or_max right);

// May convert empty key ranges to any empty key range.
key_range_t to_key_range(lower_key_bound_range kr);

void debug_print(printf_buffer_t *buf, const lower_key_bound_range &kr);

#endif  // BTREE_KEY_EDGES_HPP_
