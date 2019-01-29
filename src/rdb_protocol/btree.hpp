// Copyright 2010-2014 RethinkDB, all rights reserved.
#ifndef RDB_PROTOCOL_BTREE_HPP_
#define RDB_PROTOCOL_BTREE_HPP_

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "btree/types.hpp"
#include "concurrency/auto_drainer.hpp"
#include "rdb_protocol/datum.hpp"
#include "rdb_protocol/func.hpp"
#include "rdb_protocol/protocol.hpp"
#include "rdb_protocol/store.hpp"

namespace rocksdb { class Snapshot; }

class btree_slice_t;
enum class delete_mode_t;
class key_tester_t;
template <class> class promise_t;
struct sindex_disk_info_t;

struct rdb_modification_info_t;
struct rdb_modification_report_t;
class rdb_modification_report_cb_t;

void rdb_get(
    rockshard rocksh,
    const store_key_t &store_key,
    real_superblock_lock *superblock,
    point_read_response_t *response);

struct btree_info_t {
    btree_info_t(btree_slice_t *_slice,
                 repli_timestamp_t _timestamp,
                 const datum_string_t &_primary_key)
        : slice(_slice), timestamp(_timestamp),
          primary_key(_primary_key) {
        guarantee(slice != NULL);
    }
    btree_slice_t *const slice;
    const repli_timestamp_t timestamp;
    const datum_string_t primary_key;
};

struct btree_loc_info_t {
    btree_loc_info_t(const btree_info_t *_btree,
                     real_superblock_lock *_superblock,
                     const store_key_t *_key)
        : btree(_btree), superblock(std::move(_superblock)), key(_key) {
        guarantee(btree != nullptr);
        guarantee(superblock != nullptr);
        guarantee(key != nullptr);
    }
    const btree_info_t *const btree;
    // Holds ownership of superblock pointer (must call delete on superblock).
    real_superblock_lock *const superblock;
    const store_key_t *const key;
};

struct btree_batched_replacer_t {
    virtual ~btree_batched_replacer_t() { }
    virtual ql::datum_t replace(
        const ql::datum_t &d, size_t index) const = 0;
    virtual return_changes_t should_return_changes() const = 0;

    ql::datum_t apply_write_hook(
        const datum_string_t &pkey,
        const ql::datum_t &d,
        const ql::datum_t &res_,
        const ql::datum_t &write_timestamp,
        const counted_t<const ql::func_t> &write_hook) const;
};

batched_replace_response_t rdb_batched_replace(
    rockshard rocksh,
    const btree_info_t &info,
    real_superblock_lock *superblock,
    const std::vector<store_key_t> &keys,
    const btree_batched_replacer_t *replacer,
    rdb_modification_report_cb_t *sindex_cb,
    ql::configured_limits_t limits,
    profile::sampler_t *sampler,
    profile::trace_t *trace);

void rdb_set(rockshard rocksh,
             const store_key_t &key, ql::datum_t data,
             bool overwrite,
             btree_slice_t *slice, repli_timestamp_t timestamp,
             real_superblock_lock *superblock,
             point_write_response_t *response,
             rdb_modification_info_t *mod_info,
             profile::trace_t *trace,
             promise_t<real_superblock_lock *> *pass_back_superblock);

void rdb_delete(rockshard rocksh,
                const store_key_t &key,
                btree_slice_t *slice,
                repli_timestamp_t timestamp,
                real_superblock_lock *superblock,
                delete_mode_t delete_mode,
                point_delete_response_t *response,
                rdb_modification_info_t *mod_info,
                profile::trace_t *trace,
                promise_t<real_superblock_lock *> *pass_back_superblock);

void rdb_rget_snapshot_slice(
    const rocksdb::Snapshot *snap,
    rockshard rocksh,
    btree_slice_t *slice,
    const region_t &shard,
    const key_range_t &range,
    const optional<std::map<store_key_t, uint64_t> > &primary_keys,
    ql::env_t *ql_env,
    const ql::batchspec_t &batchspec,
    const std::vector<ql::transform_variant_t> &transforms,
    const optional<ql::terminal_variant_t> &terminal,
    sorting_t sorting,
    rget_read_response_t *response);

// TODO: So much duplication, remove this?
void rdb_rget_slice(
    rockshard rocksh,
    btree_slice_t *slice,
    const region_t &shard,
    const key_range_t &range,
    const optional<std::map<store_key_t, uint64_t> > &primary_keys,
    real_superblock_lock *superblock,
    ql::env_t *ql_env,
    const ql::batchspec_t &batchspec,
    const std::vector<ql::transform_variant_t> &transforms,
    const optional<ql::terminal_variant_t> &terminal,
    sorting_t sorting,
    rget_read_response_t *response,
    release_superblock_t release_superblock);

void rdb_rget_secondary_snapshot_slice(
    const rocksdb::Snapshot *snap,
    rockshard rocksh,
    uuid_u sindex_uuid,
    btree_slice_t *slice,
    const region_t &shard,
    const ql::datumspec_t &datumspec,
    const key_range_t &sindex_range,
    ql::env_t *ql_env,
    const ql::batchspec_t &batchspec,
    const std::vector<ql::transform_variant_t> &transforms,
    const optional<ql::terminal_variant_t> &terminal,
    const key_range_t &pk_range,
    sorting_t sorting,
    require_sindexes_t require_sindex_val,
    const sindex_disk_info_t &sindex_info,
    rget_read_response_t *response);

// TODO: So much duplication, remove this?
void rdb_rget_secondary_slice(
    rockshard rocksh,
    uuid_u sindex_uuid,
    btree_slice_t *slice,
    const region_t &shard,
    const ql::datumspec_t &datumspec,
    const key_range_t &sindex_range,
    sindex_superblock_lock *superblock,
    ql::env_t *ql_env,
    const ql::batchspec_t &batchspec,
    const std::vector<ql::transform_variant_t> &transforms,
    const optional<ql::terminal_variant_t> &terminal,
    const key_range_t &pk_range,
    sorting_t sorting,
    require_sindexes_t require_sindex_val,
    const sindex_disk_info_t &sindex_info,
    rget_read_response_t *response,
    release_superblock_t release_superblock);

void rdb_get_intersecting_slice(
        const rocksdb::Snapshot *snap,
        rockshard rocksh,
        uuid_u sindex_uuid,
        btree_slice_t *slice,
        const region_t &shard,
        const ql::datum_t &query_geometry,
        const key_range_t &sindex_range,
        ql::env_t *ql_env,
        const ql::batchspec_t &batchspec,
        const std::vector<ql::transform_variant_t> &transforms,
        const optional<ql::terminal_variant_t> &terminal,
        const key_range_t &pk_range,
        const sindex_disk_info_t &sindex_info,
        is_stamp_read_t is_stamp_read,
        rget_read_response_t *response);

void rdb_get_nearest_slice(
    const rocksdb::Snapshot *snap,
    rockshard rocksh,
    uuid_u sindex_uuid,
    btree_slice_t *slice,
    const lon_lat_point_t &center,
    double max_dist,
    uint64_t max_results,
    const ellipsoid_spec_t &geo_system,
    ql::env_t *ql_env,
    const key_range_t &pk_range,
    const sindex_disk_info_t &sindex_info,
    nearest_geo_read_response_t *response);

void rdb_distribution_get(rockshard rocksh, int keys_limit,
                          const key_range_t &key_range,
                          distribution_read_response_t *response);

/* Secondary Indexes */

struct rdb_modification_info_t {
    // TODO: Remove(?) data_pair_t wrapper (after figuring out
    // serialization/non-serialization needs).
    struct data_pair_t { ql::datum_t first; };
    data_pair_t deleted;
    data_pair_t added;
};

RDB_DECLARE_SERIALIZABLE(rdb_modification_info_t);

struct rdb_modification_report_t {
    rdb_modification_report_t() { }
    explicit rdb_modification_report_t(const store_key_t &_primary_key)
        : primary_key(_primary_key) { }

    store_key_t primary_key;
    rdb_modification_info_t info;
};

RDB_DECLARE_SERIALIZABLE(rdb_modification_report_t);

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
    reql_version_t original_reql_version;

    // This is the latest version for which evaluation of the sindex function remains
    // compatible.
    reql_version_t latest_compatible_reql_version;

    // If this is less than the current server version, we'll re-check
    // opaque_definition for compatibility and update this value and
    // `latest_compatible_reql_version` accordingly.
    reql_version_t latest_checked_reql_version;

    // To be used for new secondary indexes.
    static sindex_reql_version_info_t LATEST() {
        sindex_reql_version_info_t ret = { reql_version_t::LATEST,
                                           reql_version_t::LATEST,
                                           reql_version_t::LATEST };
        return ret;
    }
};

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

void serialize_sindex_info(write_message_t *wm,
                           const sindex_disk_info_t &info);

// Note that the behavior for how this reacts to obsolete indexes is controlled
// by the `outdated_cb`.  All other errors will throw an `archive_exc_t`.
void deserialize_sindex_info(
        const std::vector<char> &data,
        sindex_disk_info_t *info_out,
        const std::function<void(obsolete_reql_version_t)> &obsolete_cb);

// Utility function that will call deserialize_sindex_info with an `obsolete_cb`
// that will `fail_due_to_user_error` when an obsolete index is encountered.
void deserialize_sindex_info_or_crash(
        const std::vector<char> &data,
        sindex_disk_info_t *info_out)
    THROWS_ONLY(archive_exc_t);

/* An rdb_modification_cb_t is passed to BTree operations and allows them to
 * modify the secondary while they perform an operation. */
class superblock_queue_t;
class rdb_modification_report_cb_t final {
public:
    rdb_modification_report_cb_t(
            store_t *store,
            real_superblock_lock *sindex_block,
            auto_drainer_t::lock_t lock);
    ~rdb_modification_report_cb_t();

    new_mutex_in_line_t get_in_line_for_sindex();
    rwlock_in_line_t get_in_line_for_cfeed_stamp();

    void on_mod_report(rockshard rocksh,
                       const rdb_modification_report_t &mod_report,
                       bool update_pkey_cfeeds,
                       new_mutex_in_line_t *sindex_spot,
                       rwlock_in_line_t *stamp_spot);
    bool has_pkey_cfeeds(const std::vector<store_key_t> &keys);
    void finish(btree_slice_t *btree, real_superblock_lock *superblock);

private:
    void on_mod_report_sub(
        rockshard rocksh,
        const rdb_modification_report_t &mod_report,
        new_mutex_in_line_t *spot,
        cond_t *keys_available_cond,
        cond_t *done_cond,
        index_vals_t *old_keys_out,
        index_vals_t *new_keys_out);

    // TODO: Check if we use store_ and sindex_block_ after rocks-only.
    /* Fields initialized by the constructor. */
    auto_drainer_t::lock_t lock_;
    store_t *store_;
    real_superblock_lock *sindex_block_;

    /* Fields initialized by calls to on_mod_report */
    store_t::sindex_access_vector_t sindexes_;
};

void rdb_update_sindexes(
    rockshard rocksh,
    store_t *store,
    const store_t::sindex_access_vector_t &sindexes,
    const rdb_modification_report_t *modification,
    cond_t *keys_available_cond,
    index_vals_t *old_keys_out,
    index_vals_t *new_keys_out);

void post_construct_secondary_index_range(
        store_t *store,
        const std::set<uuid_u> &sindexes_to_post_construct,
        key_range_t *construction_range_inout,
        const std::function<bool(int64_t)> &check_should_abort,
        signal_t *interruptor)
    THROWS_ONLY(interrupted_exc_t);

#endif /* RDB_PROTOCOL_BTREE_HPP_ */
