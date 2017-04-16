// Copyright 2010-2014 RethinkDB, all rights reserved.
// This file has been modified by Sam Hughes.
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
INSTANTIATE_SERIALIZABLE_FOR_VERSION(user_value_t, cluster_version_t::v2_4);

// Implement actual serialization for clustering.
RDB_MAKE_SERIALIZABLE_1_FOR_CLUSTER(user_value_t, datum);

RDB_IMPL_EQUALITY_COMPARABLE_1(user_value_t, datum);


template <cluster_version_t W>
void serialize(write_message_t *wm, const table_config_t &tc) {
    table_basic_config_t basic = tc.basic;
    serialize<W>(wm, basic);

    std::vector<table_config_t::shard_t> shards = tc.shards;
    serialize<W>(wm, shards);

    std::map<std::string, sindex_config_t> sindexes = tc.sindexes;
    serialize<W>(wm, sindexes);

    optional<write_hook_config_t> write_hook = tc.write_hook;
    serialize<W>(wm, write_hook);

    write_ack_config_t write_ack_config = tc.write_ack_config;
    serialize<W>(wm, write_ack_config);

    write_durability_t durability = tc.durability;
    serialize<W>(wm, durability);
}

INSTANTIATE_SERIALIZE_FOR_CLUSTER_AND_DISK(table_config_t);

template <cluster_version_t W>
archive_result_t deserialize_table_config_pre_v2_4(
    read_stream_t *s, table_config_t *tc) {
    archive_result_t res;

    table_basic_config_t basic;
    res = deserialize<W>(s, &basic);
    if (bad(res)) { return res; }

    std::vector<table_config_t::shard_t> shards;
    res = deserialize<W>(s, &shards);
    if (bad(res)) { return res; }

    std::map<std::string, sindex_config_t> sindexes;
    res = deserialize<W>(s, &sindexes);
    if (bad(res)) { return res; }

    write_ack_config_t write_ack_config;
    res = deserialize<W>(s, &write_ack_config);
    if (bad(res)) { return res; }

    write_durability_t durability;
    res = deserialize<W>(s, &durability);
    if (bad(res)) { return res; }

    tc->basic = std::move(basic);
    tc->shards = std::move(shards);
    tc->sindexes = std::move(sindexes);
    tc->write_ack_config = std::move(write_ack_config);
    tc->durability = std::move(durability);

    return res;
}

template <cluster_version_t  W>
archive_result_t deserialize(
    read_stream_t *s, table_config_t *tc) {
    archive_result_t res;

    table_basic_config_t basic;
    res = deserialize<W>(s, &basic);
    if (bad(res)) { return res; }

    std::vector<table_config_t::shard_t> shards;
    res = deserialize<W>(s, &shards);
    if (bad(res)) { return res; }

    std::map<std::string, sindex_config_t> sindexes;
    res = deserialize<W>(s, &sindexes);
    if (bad(res)) { return res; }

    optional<write_hook_config_t> write_hook;
    res = deserialize<W>(s, &write_hook);
    if (bad(res)) { return res; }

    write_ack_config_t write_ack_config;
    res = deserialize<W>(s, &write_ack_config);
    if (bad(res)) { return res; }

    write_durability_t durability;
    res = deserialize<W>(s, &durability);
    if (bad(res)) { return res; }

    // Note that user_value serializes to nothing (gets discarded) except for v2_4_ext.
    user_value_t user_value;
    res = deserialize<W>(s, &user_value);
    if (bad(res)) { return res; }

    *tc = table_config_t{std::move(basic),
                         std::move(shards),
                         std::move(sindexes),
                         std::move(write_hook),
                         std::move(write_ack_config),
                         std::move(durability),
                         std::move(user_value)};

    return res;
}

template <>
archive_result_t deserialize<cluster_version_t::v2_1>(
    read_stream_t *s, table_config_t *tc) {
    return deserialize_table_config_pre_v2_4<cluster_version_t::v2_1>(s, tc);
}

template <>
archive_result_t deserialize<cluster_version_t::v2_2>(
    read_stream_t *s, table_config_t *tc) {
    return deserialize_table_config_pre_v2_4<cluster_version_t::v2_2>(s, tc);
}

template <>
archive_result_t deserialize<cluster_version_t::v2_3>(
    read_stream_t *s, table_config_t *tc) {
    return deserialize_table_config_pre_v2_4<cluster_version_t::v2_3>(s, tc);
}

template archive_result_t deserialize<cluster_version_t::v2_4>(
    read_stream_t *, table_config_t *);

template archive_result_t deserialize<cluster_version_t::v2_4_ext_is_latest>(
    read_stream_t *, table_config_t *);

RDB_IMPL_EQUALITY_COMPARABLE_7(table_config_t,
    basic, shards, write_hook, sindexes, write_ack_config, durability, user_value);

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

RDB_IMPL_SERIALIZABLE_1_FOR_CLUSTER(table_config_and_shards_change_t::write_hook_create_t, config);
RDB_IMPL_SERIALIZABLE_0_FOR_CLUSTER(table_config_and_shards_change_t::write_hook_drop_t);

RDB_IMPL_SERIALIZABLE_1_SINCE_v1_13(database_semilattice_metadata_t, name);
RDB_IMPL_SEMILATTICE_JOINABLE_1(database_semilattice_metadata_t, name);
RDB_IMPL_EQUALITY_COMPARABLE_1(database_semilattice_metadata_t, name);

RDB_IMPL_SERIALIZABLE_1_SINCE_v1_13(databases_semilattice_metadata_t, databases);
RDB_IMPL_SEMILATTICE_JOINABLE_1(databases_semilattice_metadata_t, databases);
RDB_IMPL_EQUALITY_COMPARABLE_1(databases_semilattice_metadata_t, databases);

flush_interval_t get_flush_interval(const table_config_t &config) {
    ql::datum_t field = config.user_value.datum.get_field("srh/flush_interval",
                                                          ql::NOTHROW);

    if (!field.has()) {
        return flush_interval_t{DEFAULT_FLUSH_INTERVAL};
    }

    if (field.get_type() == ql::datum_t::R_NUM) {
        double value_ms = 1000.0 * field.as_num();

        if (value_ms <= 0) {
            return flush_interval_t{DEFAULT_FLUSH_INTERVAL};
        }

        // We don't want any flush interval bigger than NEVER_FLUSH_INTERVAL.  (We also
        // don't want a value in milliseconds that would overflow when converted to
        // nanoseconds.  Hence this logic here.)
        static_assert(NEVER_FLUSH_INTERVAL == (0x100000000ll * 1000ll),
                      "NEVER_FLUSH_INTERVAL value changed");
        if (value_ms >= static_cast<double>(NEVER_FLUSH_INTERVAL)) {
            return flush_interval_t{NEVER_FLUSH_INTERVAL};
        }

        int64_t value_int = ceil(value_ms);
        return flush_interval_t{value_int};
    }

    if (field.get_type() == ql::datum_t::R_STR) {
        const datum_string_t &str = field.as_str();
        if (str == "never") {
            return flush_interval_t{NEVER_FLUSH_INTERVAL};
        } else {
            return flush_interval_t{DEFAULT_FLUSH_INTERVAL};
        }
    }

    return flush_interval_t{DEFAULT_FLUSH_INTERVAL};
}
