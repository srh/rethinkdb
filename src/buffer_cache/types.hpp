// Copyright 2010-2014 RethinkDB, all rights reserved.
#ifndef BUFFER_CACHE_TYPES_HPP_
#define BUFFER_CACHE_TYPES_HPP_

#include <limits.h>
#include <stdint.h>

#include "containers/archive/archive.hpp"
#include "rpc/serialize_macros.hpp"
#include "serializer/types.hpp"

// write_durability_t::INVALID is an invalid value, notably it can't be serialized.
enum class write_durability_t { INVALID, SOFT, HARD };
ARCHIVE_PRIM_MAKE_RANGED_SERIALIZABLE(write_durability_t, int8_t,
                                      write_durability_t::SOFT,
                                      write_durability_t::HARD);

#define DEFAULT_FLUSH_INTERVAL 5000

struct flush_interval_t {
    int64_t millis;
};

class txn_durability_t {
public:
    // What should the default value be?  I don't know.
    txn_durability_t() : flush_interval_ms(0) { }

    explicit txn_durability_t(write_durability_t wd)
            : flush_interval_ms(wd == write_durability_t::HARD ? -1 : 0) { }

    // HSI: Remove this entirely.
    static txn_durability_t HSI(write_durability_t wd) {
        return txn_durability_t(wd);
    }

    // The value -1 means hard durability.  0 means soft durability, and flush
    // immediately.  (I.e. old behavior, which generates so many i/o operations.)
    // INT64_MAX means don't flush any faster than you'd already want to.
    int64_t flush_interval_ms;

    // HSI: Rename or remove these...
    bool is_hard() const { return flush_interval_ms == -1; }
    write_durability_t wd() const {
        return is_hard() ? write_durability_t::HARD : write_durability_t::SOFT;
    }
    static txn_durability_t HARD() {
        return txn_durability_t(write_durability_t::HARD);
    }
    static txn_durability_t SOFT() {
        // HSI: At least some users of this (disk backed queue for example) should not
        // use the dumbest "SOFT()" setting.
        return txn_durability_t(write_durability_t::SOFT);
    }

    bool operator==(txn_durability_t other) const {
        return flush_interval_ms == other.flush_interval_ms;
    }
};

RDB_DECLARE_SERIALIZABLE(txn_durability_t);

typedef uint32_t block_magic_comparison_t;

struct block_magic_t {
    char bytes[sizeof(block_magic_comparison_t)];

    bool operator==(const block_magic_t &other) const {
        union {
            block_magic_t x;
            block_magic_comparison_t n;
        } u, v;

        u.x = *this;
        v.x = other;

        return u.n == v.n;
    }

    bool operator!=(const block_magic_t &other) const {
        return !(operator==(other));
    }
};

void debug_print(printf_buffer_t *buf, block_magic_t magic);

#endif /* BUFFER_CACHE_TYPES_HPP_ */
