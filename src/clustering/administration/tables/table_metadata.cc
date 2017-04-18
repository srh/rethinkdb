// Copyright 2010-2014 RethinkDB, all rights reserved.
// File modified by Sam Hughes (2017).
#include "clustering/administration/tables/table_metadata.hpp"

#include "clustering/administration/tables/database_metadata.hpp"
#include "containers/archive/archive.hpp"
#include "containers/archive/boost_types.hpp"
#include "containers/archive/stl_types.hpp"
#include "containers/archive/versioned.hpp"
#include "rdb_protocol/protocol.hpp"

RDB_IMPL_SERIALIZABLE_3_SINCE_v2_1(table_basic_config_t,
    name, database, primary_key);
RDB_IMPL_EQUALITY_COMPARABLE_3(table_basic_config_t,
    name, database, primary_key);

RDB_IMPL_SERIALIZABLE_3_SINCE_v2_1(table_config_t::shard_t,
    all_replicas, nonvoting_replicas, primary_replica);
RDB_IMPL_EQUALITY_COMPARABLE_3(table_config_t::shard_t,
    all_replicas, nonvoting_replicas, primary_replica);

// We start with an empty object, not null -- because a good user would set fields of
// that object.
user_value_t default_user_value() {
    return user_value_t{ql::datum_t::empty_object()};
}
// Implement zero-size serialization for v2_3 user values.

template <cluster_version_t W>
size_t serialized_size(const user_value_t &) {
    return 0;
}

template <cluster_version_t W>
void serialize(UNUSED write_message_t *wm, UNUSED const user_value_t &x) {
    // do nothing
}

template <cluster_version_t W>
archive_result_t deserialize(UNUSED read_stream_t *s, user_value_t *out) {
    *out = default_user_value();
    return archive_result_t::SUCCESS;
}

INSTANTIATE_SERIALIZABLE_FOR_VERSION(user_value_t, cluster_version_t::v2_1);
INSTANTIATE_SERIALIZABLE_FOR_VERSION(user_value_t, cluster_version_t::v2_2);
INSTANTIATE_SERIALIZABLE_FOR_VERSION(user_value_t, cluster_version_t::v2_3);

// Implement actual serialization for clustering.
RDB_MAKE_SERIALIZABLE_1_FOR_CLUSTER(user_value_t, datum);

RDB_IMPL_EQUALITY_COMPARABLE_1(user_value_t, datum);

// Note that user_value serializes to nothing (gets discarded) except for v2_3_ext.
RDB_IMPL_SERIALIZABLE_6_SINCE_v2_1(table_config_t,
    basic, shards, sindexes, write_ack_config, durability, user_value);
RDB_IMPL_EQUALITY_COMPARABLE_6(table_config_t,
    basic, shards, sindexes, write_ack_config, durability, user_value);

RDB_IMPL_SERIALIZABLE_1_SINCE_v1_16(table_shard_scheme_t, split_points);
RDB_IMPL_EQUALITY_COMPARABLE_1(table_shard_scheme_t, split_points);

RDB_IMPL_SERIALIZABLE_3_SINCE_v2_1(table_config_and_shards_t,
                                    config, shard_scheme, server_names);
RDB_IMPL_EQUALITY_COMPARABLE_3(table_config_and_shards_t,
                               config, shard_scheme, server_names);

RDB_IMPL_SERIALIZABLE_1_FOR_CLUSTER(
    table_config_and_shards_change_t::set_table_config_and_shards_t,
    new_config_and_shards);
RDB_IMPL_SERIALIZABLE_2_FOR_CLUSTER(table_config_and_shards_change_t::sindex_create_t,
    name, config);
RDB_IMPL_SERIALIZABLE_1_FOR_CLUSTER(table_config_and_shards_change_t::sindex_drop_t,
    name);
RDB_IMPL_SERIALIZABLE_3_FOR_CLUSTER(table_config_and_shards_change_t::sindex_rename_t,
    name, new_name, overwrite);

RDB_IMPL_SERIALIZABLE_1_SINCE_v1_13(database_semilattice_metadata_t, name);
RDB_IMPL_SEMILATTICE_JOINABLE_1(database_semilattice_metadata_t, name);
RDB_IMPL_EQUALITY_COMPARABLE_1(database_semilattice_metadata_t, name);

RDB_IMPL_SERIALIZABLE_1_SINCE_v1_13(databases_semilattice_metadata_t, databases);
RDB_IMPL_SEMILATTICE_JOINABLE_1(databases_semilattice_metadata_t, databases);
RDB_IMPL_EQUALITY_COMPARABLE_1(databases_semilattice_metadata_t, databases);

txn_durability_t get_txn_durability(const table_config_t &config) {
    // HSI: Use user_value.
    return txn_durability_t::HSI(config.durability);
}
