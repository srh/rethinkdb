#ifndef REGION_REGION_MAP_HPP_
#define REGION_REGION_MAP_HPP_

#include <algorithm>
#include <utility>
#include <vector>

#include "utils.hpp"
#include "containers/archive/stl_types.hpp"
#include "containers/range_map.hpp"
#include "region/region.hpp"

/* `region_map_t` is a mapping from contiguous non-overlapping `region_t`s to `value_t`s.
It will automatically merge contiguous regions with the same value (in most cases).
Internally, it's implemented as two nested `range_map_t`s. */
template <class value_t>
class region_map_t {
private:
    typedef key_range_t::right_bound_t key_edge_t;
    typedef range_map_t<key_edge_t, value_t> key_range_map_t;

public:
    typedef value_t mapped_type;

    explicit region_map_t(
            region_t r = region_t::universe(),
            value_t v = value_t()) THROWS_NOTHING :
        inner(
            key_edge_t(r.left),
            r.right,
            std::move(v))
        { }

    static region_map_t from_unordered_fragments(
            std::vector<region_t> &&regions,
            std::vector<value_t> &&values) {
        region_t domain;
        region_join_result_t res = region_join(regions, &domain);
        guarantee(res == REGION_JOIN_OK);
        region_map_t map(domain);
        for (size_t i = 0; i < regions.size(); ++i) {
            map.update(regions[i], values[i]);
        }
        return map;
    }

    static region_map_t empty() {
        region_map_t r;
        r.inner = key_range_map_t(key_edge_t());
        return r;
    }

    region_map_t(const region_map_t &) = default;
    region_map_t(region_map_t &&) = default;
    region_map_t &operator=(const region_map_t &) = default;
    region_map_t &operator=(region_map_t &&) = default;

    region_t get_domain() const THROWS_NOTHING {
        if (inner.empty_domain()) {
            return region_t::empty();
        } else {
            region_t r;
            r.left = inner.left_edge().key();
            r.right = inner.right_edge();
            return r;
        }
    }

    const value_t &lookup(const store_key_t &key) const {
        return inner.lookup(key_edge_t(key));
    }

    /* Calls `cb` for a set of (subregion, value) pairs that cover all of `region`. The
    signature of `cb` is:
        void cb(const region_t &, const value_t &)
    */
    template<class callable_t>
    void visit(const region_t &region, const callable_t &cb) const {
        inner.visit(key_edge_t(region.left), region.right,
            [&](const key_edge_t &l, const key_edge_t &r, const value_t &value) {
                key_range_t subrange;
                subrange.left = l.key();
                subrange.right = r;
                cb(subrange, value);
            });
    }

    /* Derives a new `region_map_t` from the given region of this `region_map_t` by
    applying `cb` to transform each value. The signature of `cb` is:
        value2_t cb(const value_t &);
    and the return value will have type `region_map_t<value2_t>` and domain equal to
    `region`. */
    template<class callable_t>
    auto map(const region_t &region, const callable_t &cb) const
            -> region_map_t<typename std::decay<
                typename std::result_of<callable_t(value_t)>::type>::type> {
        return region_map_t<typename std::decay<
                typename std::result_of<callable_t(value_t)>::type>::type>(
            inner.map(key_edge_t(region.left), region.right, cb));
    }

    /* Like `map()`, but the mapping can take into account the original location and can
    produce non-homogeneous results from a homogeneous input. The signature of `cb` is:
        region_map_t<value2_t> cb(const region_t &region, const value_t &);
    and the return value will have type `region_map_t<value2_t>` and domain equal to
    `region`. */
    template<class callable_t>
    auto map_multi(const region_t &region, const callable_t &cb) const
            -> region_map_t<typename std::decay<
                typename std::result_of<callable_t(region_t, value_t)>::type>
                ::type::mapped_type> {
        typedef typename std::decay<
            typename std::result_of<callable_t(region_t, value_t)>::type>
            ::type::mapped_type result_t;
        return region_map_t<result_t>(
            inner.map_multi(key_edge_t(region.left), region.right,
            [&](const key_edge_t &l, const key_edge_t &r, const value_t &value) {
                key_range_t sub_reg;
                sub_reg.left = l.key();
                sub_reg.right = r;
                return cb(sub_reg, value).inner;
            }));
    }

    /* Copies a subset of this `region_map_t` into a new `region_map_t`. */
    MUST_USE region_map_t mask(const region_t &region) const {
        return region_map_t(
            inner.mask(
                key_edge_t(region.left),
                region.right));
    }

    bool operator==(const region_map_t &other) const {
        return inner == other.inner;
    }
    bool operator!=(const region_map_t &other) const {
        return inner != other.inner;
    }

    /* Overwrites part or all of this `region_map_t` with the contents of the given
    `region_map_t`. This does not change the domain of this `region_map_t`; `new_values`
    must lie entirely within the current domain. */
    void update(const region_map_t& new_values) {
        rassert(region_is_superset(get_domain(), new_values.get_domain()));
        inner.update(key_range_map_t(new_values.inner));
    }

    /* `update()` sets the value for `r` to `v`. */
    void update(const region_t &r, const value_t &v) {
        rassert(region_is_superset(get_domain(), r));
        inner.update(key_edge_t(r.left), r.right, value_t(v));
    }

    /* Merges two `region_map_t`s that cover the same part of the hash-space and adjacent
    ranges of the key-space. */
    void extend_keys_right(region_map_t &&new_values) {
        if (inner.empty_domain()) {
            *this = std::move(new_values);
        } else {
            inner.extend_right(std::move(new_values.inner));
        }
    }

    /* Applies `cb` to every value in the given region. If some sub-region lies partially
    inside and partially outside of `region`, then it will be split and `cb` will only be
    applied to the part that lies inside `region`. The signature of `cb` is:
        void cb(const region_t &, value_t *);
    */
    template<class callable_t>
    void visit_mutable(const region_t &region, const callable_t &cb) {
        inner.visit_mutable(key_edge_t(region.left), region.right,
            [&](const key_edge_t &l, const key_edge_t &r, value_t *value) {
                key_range_t sub_reg;
                sub_reg.left = l.key();
                sub_reg.right = r;
                cb(sub_reg, value);
            });
    }

private:
    template<class other_value_t>
    friend class region_map_t;

    template<class V>
    friend void debug_print(printf_buffer_t *buf, const region_map_t<V> &map);

    template<cluster_version_t W, class V>
    friend void serialize(write_message_t *wm, const region_map_t<V> &map);

    template<cluster_version_t W, class V>
    friend MUST_USE archive_result_t deserialize(read_stream_t *s, region_map_t<V> *map);

    explicit region_map_t(key_range_map_t &&_inner) :
        inner(_inner) { }

    key_range_map_t inner;
};

template <class V>
void debug_print(printf_buffer_t *buf, const region_map_t<V> &map) {
    buf->appendf("rmap{");
    debug_print(buf, map.inner);
    buf->appendf("}");
}

template<cluster_version_t W, class V>
void serialize(write_message_t *wm, const region_map_t<V> &map) {
    static_assert(W == cluster_version_t::v2_5_is_latest,
        "serialize() is only supported for the latest version");
    serialize<W>(wm, map.inner);
}

template<cluster_version_t W, class V>
MUST_USE archive_result_t deserialize(read_stream_t *s, region_map_t<V> *map) {
    static_assert(W == cluster_version_t::v2_5_is_latest,
        "deserialize() is only supported for the latest version");
    archive_result_t res;
    res = deserialize<W>(s, &map->inner);
    if (bad(res)) { return res; }
    return archive_result_t::SUCCESS;
}

#endif  // REGION_REGION_MAP_HPP_
