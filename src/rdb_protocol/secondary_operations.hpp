// Copyright 2010-2015 RethinkDB, all rights reserved.
#ifndef RDB_PROTOCOL_SECONDARY_OPERATIONS_HPP_
#define RDB_PROTOCOL_SECONDARY_OPERATIONS_HPP_

#include <map>
#include <string>
#include <vector>

#include "btree/keys.hpp"
#include "buffer_cache/types.hpp"
#include "containers/archive/archive.hpp"
#include "containers/uuid.hpp"
#include "rdb_protocol/wire_func.hpp"
#include "rpc/serialize_macros.hpp"

class rockshard;

class real_superblock_lock;

// The query evaluation reql version information that we store with each secondary
// index function.
struct sindex_reql_version_info_t {
    // Generally speaking, original_reql_version <= latest_compatible_reql_version <=
    // latest_checked_reql_version.  When a new sindex is created, the values are the
    // same.  When a new version of RethinkDB gets run, latest_checked_reql_version
    // will get updated, and latest_compatible_reql_version will get updated if the
    // sindex function is compatible with a later version than the original value of
    // `latest_checked_reql_version`.

    // The original ReQL version of the sindex function.  The value here never
    // changes.  This might become useful for tracking down some bugs or fixing them
    // in-place, or performing a desperate reverse migration.
    importable_reql_version_t original_reql_version;

    // This is the latest version for which evaluation of the sindex function remains
    // compatible.
    reql_version_t latest_compatible_reql_version;

    // If this is less than the current server version, we'll re-check
    // opaque_definition for compatibility and update this value and
    // `latest_compatible_reql_version` accordingly.
    reql_version_t latest_checked_reql_version;

    // To be used for new secondary indexes.
    static sindex_reql_version_info_t LATEST() {
        sindex_reql_version_info_t ret = { importable_reql_version_t::LATEST,
                                           reql_version_t::LATEST,
                                           reql_version_t::LATEST };
        return ret;
    }
};

RDB_DECLARE_SERIALIZABLE(sindex_reql_version_info_t);

enum class sindex_multi_bool_t { SINGLE = 0, MULTI = 1};
enum class sindex_geo_bool_t { REGULAR = 0, GEO = 1};

ARCHIVE_PRIM_MAKE_RANGED_SERIALIZABLE(sindex_multi_bool_t, int8_t,
        sindex_multi_bool_t::SINGLE, sindex_multi_bool_t::MULTI);
ARCHIVE_PRIM_MAKE_RANGED_SERIALIZABLE(sindex_geo_bool_t, int8_t,
        sindex_geo_bool_t::REGULAR, sindex_geo_bool_t::GEO);

struct sindex_disk_info_t {
    sindex_disk_info_t() { }
    sindex_disk_info_t(const ql::map_wire_func_t &_mapping,
                       const sindex_reql_version_info_t &_mapping_version_info,
                       sindex_multi_bool_t _multi,
                       sindex_geo_bool_t _geo) :
        mapping(_mapping), mapping_version_info(_mapping_version_info),
        multi(_multi), geo(_geo) { }
    ql::map_wire_func_t mapping;
    sindex_reql_version_info_t mapping_version_info;
    sindex_multi_bool_t multi;
    sindex_geo_bool_t geo;
};

RDB_DECLARE_SERIALIZABLE(sindex_disk_info_t);

struct secondary_index_t {
    secondary_index_t()
        : needs_post_construction_range(key_range_t::universe()),
          being_deleted(false),
          id(generate_uuid()) { }

    /* Whether the index still needs to be post constructed, and/or is being deleted.
     Note that an index can be in any combination of those states. */
    key_range_t needs_post_construction_range;
    bool being_deleted;

    /* Note that this is even still relevant if the index is being deleted. In that case
     it tells us whether the index had completed post constructing before it got deleted
     or not. That is relevant because once an index got post-constructed, there can be
     snapshotted read queries that are still accessing it, and we must detach any
     values that we are deleting from the index.
     If on the other hand the index never finished post-construction, we must not detach
     values because they might be pointing to blocks that no longer exist (in general a
     not fully constructed index can be in an inconsistent state). */
    bool post_construction_complete() const {
        return needs_post_construction_range.is_empty();
    }

    /* Determines whether it's ok to query the index. */
    bool is_ready() const {
        return post_construction_complete() && !being_deleted;
    }

    /* Contains the ReQL info defining sindex query behavior. */
    sindex_disk_info_t definition;

    /* Sindexes contain a uuid_u to prevent a rapid deletion and recreation of
     * a sindex with the same name from tricking a post construction in to
     * switching targets. This issue is described in more detail here:
     * https://github.com/rethinkdb/rethinkdb/issues/657 */
    uuid_u id;
};

RDB_DECLARE_SERIALIZABLE(secondary_index_t);

struct sindex_name_t {
    sindex_name_t()
        : name(""), being_deleted(false) { }
    explicit sindex_name_t(const std::string &n)
        : name(n), being_deleted(false) { }

    bool operator<(const sindex_name_t &other) const {
        return (being_deleted && !other.being_deleted) ||
               (being_deleted == other.being_deleted && name < other.name);
    }
    bool operator==(const sindex_name_t &other) const {
        return being_deleted == other.being_deleted && name == other.name;
    }

    std::string name;
    // This additional bool in `sindex_name_t` makes sure that the name
    // of an index that's being deleted can never conflict with any newly created
    // index.
    bool being_deleted;
};

RDB_DECLARE_SERIALIZABLE(sindex_name_t);

//Secondary Index functions

/* Note if this function is called after secondary indexes have been added it
 * will leak blocks (and also make those secondary indexes unusable.) There's
 * no reason to ever do this. */
void initialize_secondary_indexes(
    rockshard rocksh, real_superblock_lock *sindex_block);

bool get_secondary_index(rockshard rocksh, real_superblock_lock *sindex_block,
                         const sindex_name_t &name,
                         secondary_index_t *sindex_out);

bool get_secondary_index(rockshard rocksh, real_superblock_lock *sindex_block, uuid_u id,
                         secondary_index_t *sindex_out);

void get_secondary_indexes(rockshard rocksh, real_superblock_lock *sindex_block,
                           std::map<sindex_name_t, secondary_index_t> *sindexes_out);

// TODO: Check if any of these functions are unused.
/* Overwrites existing values with the same id. */
void set_secondary_index(rockshard rocksh, real_superblock_lock *sindex_block,
                         const sindex_name_t &name, const secondary_index_t &sindex);

/* Must be used to overwrite an already existing sindex. */
// TODO: Unclear why we have redundant id/sindex.id parameters.
void set_secondary_index(rockshard rocksh, real_superblock_lock *sindex_block, uuid_u id,
                         const secondary_index_t &sindex);

void set_secondary_indexes(rockshard rocksh, real_superblock_lock *sindex_block,
        const std::map<sindex_name_t, secondary_index_t> &sindexes);

// XXX note this just drops the entry. It doesn't cleanup the btree that it points
// to. `drop_sindex` Does both and should be used publicly.
bool delete_secondary_index(rockshard rocksh,
                            real_superblock_lock *sindex_block, const sindex_name_t &name);

#endif /* RDB_PROTOCOL_SECONDARY_OPERATIONS_HPP_ */
