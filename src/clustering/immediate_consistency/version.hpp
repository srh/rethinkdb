// Copyright 2010-2015 RethinkDB, all rights reserved.
#ifndef CLUSTERING_IMMEDIATE_CONSISTENCY_VERSION_HPP_
#define CLUSTERING_IMMEDIATE_CONSISTENCY_VERSION_HPP_

#include <map>
#include <set>

#include "containers/uuid.hpp"
#include "rpc/serialize_macros.hpp"
#include "timestamps.hpp"
#include "utils.hpp"

/* The type `version_t` uniquely identifies the state of some region of a RethinkDB table
at some point in time. Every read operation that passes through a `broadcaster_t` will
get all its data from the version that the broadcaster is at at the time that the read
arrives. Every write operation that passes through a `broadcaster_t` will transition the
data from one `version_t` to the next.

`version_t` internally consists of two parts: a `branch_id_t` and a timestamp. For the
`version_t::zero()`, which is the version of the initial empty database, the
`branch_id_t` is nil and the timestamp is 0. Any other version than `version_t::zero()`
belongs to a `broadcaster_t`. The `branch_id_t` will be a new UUID that was generated
when the `broadcaster_t` was created, and the timestamp will be a number that the
`broadcaster_t` increments every time a write operation passes through it. (Warning: The
timestamp is usually not zero for a new `broadcaster_t`.) */

// We need ATTR_PACKED for some reinterpret_cast<const version_t *> operations.
// (Previously, we _should_ have had ATTR_PACKED for use with binary_blob_t.)
ATTR_PACKED(class version_t {
public:
    version_t() { }
    version_t(branch_id_t bid, state_timestamp_t ts) :
        branch(bid), timestamp(ts) { }
    static version_t zero() {
        return version_t(nil_uuid(), state_timestamp_t::zero());
    }

    bool operator==(const version_t &v) const{
        return branch == v.branch && timestamp == v.timestamp;
    }
    bool operator!=(const version_t &v) const {
        return !(*this == v);
    }

    branch_id_t branch;
    state_timestamp_t timestamp;
});

RDB_DECLARE_SERIALIZABLE(version_t);

void debug_print(printf_buffer_t *buf, const version_t &v);

#endif /* CLUSTERING_IMMEDIATE_CONSISTENCY_VERSION_HPP_ */
