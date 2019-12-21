#ifndef BTREE_KEY_OR_MAX_HPP_
#define BTREE_KEY_OR_MAX_HPP_

#include "btree/key_edges.hpp"
#include "btree/keys.hpp"

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
};

RDB_DECLARE_SERIALIZABLE_FOR_CLUSTER(key_or_max);

static const key_or_max key_or_max_min = key_or_max::min();
static const key_or_max key_or_max_infinity = key_or_max::infinity();

void debug_print(printf_buffer_t *buf, const key_or_max &km);

#endif  // BTREE_KEY_OR_MAX_HPP_
