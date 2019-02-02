// Copyright 2010-2014 RethinkDB, all rights reserved.
#ifndef BTREE_KEYS_HPP_
#define BTREE_KEYS_HPP_

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "arch/compiler.hpp"
#include "config/args.hpp"
#include "containers/archive/archive.hpp"
#include "rpc/serialize_macros.hpp"

#if defined(__GNUC__) && (100 * __GNUC__ + __GNUC_MINOR__ >= 406)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
#endif

enum class sorting_t {
    UNORDERED,
    ASCENDING,
    DESCENDING
};
// UNORDERED sortings aren't reversed
bool reversed(sorting_t sorting);

template<class T>
bool is_better(const T &a, const T &b, sorting_t sorting) {
    if (!reversed(sorting)) {
        return a < b;
    } else {
        return b < a;
    }
}

// Fast string compare
int sized_strcmp(const uint8_t *str1, int len1, const uint8_t *str2, int len2);

struct store_key_t {
public:
    store_key_t() {
        set_size(0);
    }

    store_key_t(int sz, const uint8_t *buf) {
        assign(sz, buf);
    }

    explicit store_key_t(std::string &&s) : str_(std::move(s)) {
        rassert(str_.size() <= MAX_KEY_SIZE);
    }

    explicit store_key_t(const std::string &s) : str_(s) {
        rassert(str_.size() <= MAX_KEY_SIZE);
    }

    // TODO: Any callers?
    void set_size(int s) {
        rassert(s <= MAX_KEY_SIZE);
        str_.resize(s);
    }
    int size() const { return str_.size(); }
    const uint8_t *data() const { return reinterpret_cast<const uint8_t *>(str_.data()); }

    void assign(int sz, const uint8_t *buf) {
        str_.assign(reinterpret_cast<const char *>(buf), sz);
        rassert(sz <= MAX_KEY_SIZE);
    }

    void assign(const store_key_t &key) {
        str_ = key.str_;
    }

    static store_key_t min() {
        return store_key_t();
    }

    std::string &str() { return str_; }
    const std::string &str() const { return str_; }

    static store_key_t max() {
        uint8_t buf[MAX_KEY_SIZE];
        for (int i = 0; i < MAX_KEY_SIZE; i++) {
            buf[i] = 255;
        }
        return store_key_t(MAX_KEY_SIZE, buf);
    }

    bool increment() {
        if (str_.size() < MAX_KEY_SIZE) {
            str_.push_back(0);
            return true;
        }
        while (str_.size() > 0 && static_cast<uint8_t>(str_.back()) == 255) {
            str_.pop_back();
        }
        if (str_.empty()) {
            /* We were the largest possible key. Oops. Restore our previous
            state and return `false`. */
            *this = store_key_t::max();
            return false;
        }
        str_.back() = 1 + static_cast<uint8_t>(str_.back());
        return true;
    }

    // TODO: It would be nice if we never called this.
    bool decrement() {
        if (str_.empty()) {
            return false;
        } else if (str_.back() != 0) {
            str_.back() = static_cast<uint8_t>(str_.back()) - 1;
            for (int i = str_.size(); i < MAX_KEY_SIZE; i++) {
                str_.push_back(static_cast<char>(255));
            }
            return true;
        } else {
            str_.pop_back();
            return true;
        }
    }

    int compare(const store_key_t& k) const {
        return sized_strcmp(data(), size(), k.data(), k.size());
    }

    // The wire format of serialize_for_metainfo must not change.
    void serialize_for_metainfo(write_message_t *wm) const {
        uint8_t sz = size();
        serialize_universal(wm, sz);
        wm->append(data(), sz);
    }

    archive_result_t deserialize_for_metainfo(read_stream_t *s) {
        uint8_t sz;
        archive_result_t res = deserialize_universal(s, &sz);
        if (bad(res)) { return res; }
        std::string buf;
        buf.resize(sz);
        int64_t num_read = force_read(s, &buf[0], sz);
        if (num_read == -1) {
            return archive_result_t::SOCK_ERROR;
        }
        if (num_read < sz) {
            return archive_result_t::SOCK_EOF;
        }
        rassert(num_read == sz);
        str_ = std::move(buf);
        return archive_result_t::SUCCESS;
    }

    template <cluster_version_t W>
    friend void serialize(write_message_t *wm, const store_key_t &sk) {
        sk.serialize_for_metainfo(wm);
    }

    template <cluster_version_t W>
    friend archive_result_t deserialize(read_stream_t *s, store_key_t *sk) {
        return sk->deserialize_for_metainfo(s);
    }

private:
    std::string str_;
};

static const store_key_t store_key_max = store_key_t::max();
static const store_key_t store_key_min = store_key_t::min();

inline bool operator==(const store_key_t &k1, const store_key_t &k2) {
    return k1.size() == k2.size() && memcmp(k1.data(), k2.data(), k1.size()) == 0;
}

inline bool operator!=(const store_key_t &k1, const store_key_t &k2) {
    return !(k1 == k2);
}

inline bool operator<(const store_key_t &k1, const store_key_t &k2) {
    return k1.compare(k2) < 0;
}

inline bool operator>(const store_key_t &k1, const store_key_t &k2) {
    return k2 < k1;
}

inline bool operator<=(const store_key_t &k1, const store_key_t &k2) {
    return k1.compare(k2) <= 0;
}

inline bool operator>=(const store_key_t &k1, const store_key_t &k2) {
    return k2 <= k1;
}

bool unescaped_str_to_key(const char *str, int len, store_key_t *buf);
std::string key_to_unescaped_str(const store_key_t &key);
std::string key_to_debug_str(const store_key_t &key);

/* `key_range_t` represents a contiguous set of keys. */
class key_range_t {
public:
    /* If `right.unbounded`, then the range contains all keys greater than or
    equal to `left`. If `right.bounded`, then the range contains all keys
    greater than or equal to `left` and less than `right.key`. */
    struct right_bound_t {
        right_bound_t() : unbounded(true) { }

        explicit right_bound_t(store_key_t k) : unbounded(false), internal_key(k) { }
        static right_bound_t make_unbounded() {
            right_bound_t rb;
            rb.unbounded = true;
            return rb;
        }

        bool increment() {
            if (unbounded) {
                return false;
            } else {
                if (!internal_key.increment()) {
                    unbounded = true;
                }
                return true;
            }
        }

        store_key_t &key() {
            rassert(!unbounded);
            return internal_key;
        }
        const store_key_t &key() const {
            rassert(!unbounded);
            return internal_key;
        }
        const store_key_t &key_or_max() const {
            return unbounded ? store_key_max : internal_key;
        }

        bool unbounded;

        /* This is meaningless if `unbounded` is true. Usually you should call `key()`
        instead of accessing this directly. */
        store_key_t internal_key;
    };

    const store_key_t &right_or_max() const {
        return right.key_or_max();
    }

    // This enum class / static constexpr stuff is to silence a warning that "open"
    // shadows a global variable named open.  This used to be "enum bound_t".  Removing
    // the static constexpr definitions would be acceptable.
    enum class bound_t {
        open,
        closed,
        none
    };

    static constexpr bound_t open = bound_t::open;
    static constexpr bound_t closed = bound_t::closed;
    static constexpr bound_t none = bound_t::none;

    key_range_t();   /* creates a range containing no keys */
    key_range_t(bound_t lm, const store_key_t &l,
                bound_t rm, const store_key_t &r);

    template<class T>
    static key_range_t one_key(const T &key) {
        return key_range_t(closed, key, closed, key);
    }

    static key_range_t empty() THROWS_NOTHING {
        return key_range_t();
    }

    static key_range_t universe() THROWS_NOTHING {
        store_key_t k;
        return key_range_t(key_range_t::none, k, key_range_t::none, k);
    }

    bool is_empty() const {
        if (right.unbounded) {
            return false;
        } else {
            rassert(left <= right.key());
            return left == right.key();
        }
    }

    // TODO: rename these all to `contains` for consistency with other classes.
    bool contains_key(const store_key_t& key) const {
        bool left_ok = left <= key;
        bool right_ok = right.unbounded || key < right.key();
        return left_ok && right_ok;
    }

    bool contains_key(const uint8_t *key, uint8_t size) const {
        bool left_ok = sized_strcmp(left.data(), left.size(), key, size) <= 0;
        bool right_ok = right.unbounded ||
            sized_strcmp(key, size, right.key().data(), right.key().size()) < 0;
        return left_ok && right_ok;
    }

    std::string print() const;
    bool is_superset(const key_range_t &other) const;
    bool overlaps(const key_range_t &other) const;
    key_range_t intersection(const key_range_t &other) const;

    store_key_t left;
    right_bound_t right;

private:
    void init(bound_t lm, const store_key_t &l,
              bound_t rm, const store_key_t &r);
};

RDB_DECLARE_SERIALIZABLE(key_range_t::right_bound_t);
RDB_DECLARE_SERIALIZABLE(key_range_t);

// Serialization with stable wire format for metainfo blob.
void serialize_for_metainfo(write_message_t *wm, const key_range_t &kr);
MUST_USE archive_result_t deserialize_for_metainfo(read_stream_t *s, key_range_t *out);

void debug_print(printf_buffer_t *buf, const store_key_t &k);
void debug_print(printf_buffer_t *buf, const store_key_t *k);
void debug_print(printf_buffer_t *buf, const key_range_t::right_bound_t &rb);
void debug_print(printf_buffer_t *buf, const key_range_t &kr);
std::string key_range_to_string(const key_range_t &kr);

bool operator==(const key_range_t::right_bound_t &a, const key_range_t::right_bound_t &b);
bool operator!=(const key_range_t::right_bound_t &a, const key_range_t::right_bound_t &b);
bool operator<(const key_range_t::right_bound_t &a, const key_range_t::right_bound_t &b);
bool operator<=(const key_range_t::right_bound_t &a, const key_range_t::right_bound_t &b);
bool operator>(const key_range_t::right_bound_t &a, const key_range_t::right_bound_t &b);
bool operator>=(const key_range_t::right_bound_t &a, const key_range_t::right_bound_t &b);

bool operator==(key_range_t, key_range_t) THROWS_NOTHING;
bool operator!=(key_range_t, key_range_t) THROWS_NOTHING;
bool operator<(const key_range_t &, const key_range_t &) THROWS_NOTHING;

#if defined(__GNUC__) && (100 * __GNUC__ + __GNUC_MINOR__ >= 406)
#pragma GCC diagnostic pop
#endif

#endif // BTREE_KEYS_HPP_
