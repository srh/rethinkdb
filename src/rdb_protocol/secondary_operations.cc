// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "rdb_protocol/secondary_operations.hpp"

#include "rocksdb/options.h"

#include "btree/operations.hpp"
#include "buffer_cache/alt.hpp"
#include "containers/archive/stl_types.hpp"
#include "containers/archive/string_stream.hpp"
#include "containers/archive/vector_stream.hpp"
#include "containers/archive/versioned.hpp"
#include "debug.hpp"
#include "protocol_api.hpp"
#include "rockstore/store.hpp"
#include "utils.hpp"

RDB_IMPL_SERIALIZABLE_3_SINCE_v2_4(
        sindex_reql_version_info_t, original_reql_version, latest_compatible_reql_version, latest_checked_reql_version
);

// TODO: We'll have to be precise with sindex_create.
RDB_IMPL_SERIALIZABLE_4_SINCE_v2_4(
        sindex_disk_info_t, mapping, mapping_version_info, multi, geo);



RDB_IMPL_SERIALIZABLE_4_SINCE_v2_4(
        secondary_index_t, definition,
        needs_post_construction_range, being_deleted, id);

RDB_IMPL_SERIALIZABLE_2_SINCE_v1_13(sindex_name_t, name, being_deleted);


void get_secondary_indexes_internal(
        rockshard rocksh,
        real_superblock_lock *superblock,
        std::map<sindex_name_t, secondary_index_t> *sindexes_out) {
    superblock->read_acq_signal()->wait_lazily_ordered();

    std::string kv_location = rockstore::table_sindex_map(rocksh.table_id);

    // TODO: Some vestigial code with a guarantee here -- presumably to be cleaned up later.
    std::string rocks_sindex_blob;
    rocksdb::Status status = superblock->wait_read_batch()->GetFromBatch(rocksdb::DBOptions(), kv_location, &rocks_sindex_blob);
    guarantee(!status.ok(), "Expecting no get-after-set of sindex_map");

    rocks_sindex_blob = rocksh.rocks->read(kv_location);

    string_read_stream_t stream(std::move(rocks_sindex_blob), 0);
    archive_result_t res = deserialize<cluster_version_t::v2_5_is_latest_disk>(&stream, sindexes_out);
    guarantee_deserialization(res, "sindex_map");
}

void set_secondary_indexes_internal(
        rockshard rocksh,
        real_superblock_lock *superblock,
        const std::map<sindex_name_t, secondary_index_t> &sindexes) {
    superblock->write_acq_signal()->wait_lazily_ordered();

    // TODO: rocksdb transactionality
    std::string sindex_rocks_blob = serialize_to_string<cluster_version_t::LATEST_DISK>(sindexes);
    std::string sindex_rocks_key = rockstore::table_sindex_map(rocksh.table_id);
    superblock->wait_write_batch()->Put(sindex_rocks_key, sindex_rocks_blob);
}

void initialize_secondary_indexes(rockshard rocksh,
                                  real_superblock_lock *sindex_block) {
    set_secondary_indexes_internal(rocksh, sindex_block,
                                   std::map<sindex_name_t, secondary_index_t>());
}

bool get_secondary_index(rockshard rocksh, real_superblock_lock *sindex_block, const sindex_name_t &name,
                         secondary_index_t *sindex_out) {
    std::map<sindex_name_t, secondary_index_t> sindex_map;

    get_secondary_indexes_internal(rocksh, sindex_block, &sindex_map);

    auto it = sindex_map.find(name);
    if (it != sindex_map.end()) {
        *sindex_out = it->second;
        return true;
    } else {
        return false;
    }
}

bool get_secondary_index(rockshard rocksh, real_superblock_lock *sindex_block, uuid_u id,
                         secondary_index_t *sindex_out) {
    std::map<sindex_name_t, secondary_index_t> sindex_map;

    get_secondary_indexes_internal(rocksh, sindex_block, &sindex_map);
    for (auto it = sindex_map.begin(); it != sindex_map.end(); ++it) {
        if (it->second.id == id) {
            *sindex_out = it->second;
            return true;
        }
    }
    return false;
}

void get_secondary_indexes(rockshard rocksh, real_superblock_lock *sindex_block,
                           std::map<sindex_name_t, secondary_index_t> *sindexes_out) {
    get_secondary_indexes_internal(rocksh, sindex_block, sindexes_out);
}

void set_secondary_index(rockshard rocksh,
                         real_superblock_lock *sindex_block, const sindex_name_t &name,
                         const secondary_index_t &sindex) {
    std::map<sindex_name_t, secondary_index_t> sindex_map;
    get_secondary_indexes_internal(rocksh, sindex_block, &sindex_map);

    /* We insert even if it already exists overwriting the old value. */
    sindex_map[name] = sindex;
    set_secondary_indexes_internal(rocksh, sindex_block, sindex_map);
}

void set_secondary_index(rockshard rocksh,
                         real_superblock_lock *sindex_block, uuid_u id,
                         const secondary_index_t &sindex) {
    std::map<sindex_name_t, secondary_index_t> sindex_map;
    get_secondary_indexes_internal(rocksh, sindex_block, &sindex_map);

    for (auto it = sindex_map.begin(); it != sindex_map.end(); ++it) {
        if (it->second.id == id) {
            guarantee(sindex.id == id, "This shouldn't change the id.");
            it->second = sindex;
        }
    }
    set_secondary_indexes_internal(rocksh, sindex_block, sindex_map);
}

void set_secondary_indexes(rockshard rocksh, real_superblock_lock *sindex_block,
        const std::map<sindex_name_t, secondary_index_t> &sindexes) {
    set_secondary_indexes_internal(rocksh, sindex_block, sindexes);
}

bool delete_secondary_index(rockshard rocksh,
                            real_superblock_lock *sindex_block, const sindex_name_t &name) {
    std::map<sindex_name_t, secondary_index_t> sindex_map;
    get_secondary_indexes_internal(rocksh, sindex_block, &sindex_map);

    if (sindex_map.erase(name) == 1) {
        set_secondary_indexes_internal(rocksh, sindex_block, sindex_map);
        return true;
    } else {
        return false;
    }
}

