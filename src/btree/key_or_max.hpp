#ifndef BTREE_KEY_OR_MAX_HPP_
#define BTREE_KEY_OR_MAX_HPP_

#include "btree/key_edges.hpp"
#include "btree/keys.hpp"

// Key or max extends the keyspace with the value +infinity.  This is used in
// place of code which previously used "\xFF\xFF..." in places.  The purpose of
// this type is (or was) to replace such uses of store_key_t and the max key
// value in a type safe manner.  See also lower_key_bound, which replaces
// another set of uses of the max store key values -- and these types do
// interact slightly -- but key_or_max does not specifically represent a key
// boundary.  (In its usage in limit_read_last_key, it might represent a
// boundary either on the low or high side of the key, depending on which way we
// are traversing.)
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

    lower_key_bound make_lower_bound() const {
        lower_key_bound ret;
        ret.infinite = infinite;
        ret.key = key;
        return ret;
    }

    static key_or_max from_right_bound(const key_range_t::right_bound_t &rb) {
        key_or_max ret;
        ret.infinite = rb.unbounded;
        ret.key = rb.internal_key;
        return ret;
    }

    static key_or_max from_lower_bound(const lower_key_bound &kb) {
        key_or_max ret;
        ret.infinite = kb.infinite;
        ret.key = kb.key;
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

static const key_or_max key_or_max_min = key_or_max::min();
static const key_or_max key_or_max_infinity = key_or_max::infinity();

void debug_print(printf_buffer_t *buf, const key_or_max &km);

inline key_range_t half_open(store_key_t left, key_or_max right) {
    key_range_t ret;
    ret.left = std::move(left);
    ret.right = std::move(right).to_right_bound();
    return ret;
}


#endif  // BTREE_KEY_OR_MAX_HPP_
