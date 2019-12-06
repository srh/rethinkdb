// Copyright 2010-2015 RethinkDB, all rights reserved.
#include "rdb_protocol/btree.hpp"

#include <algorithm>
#include <functional>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include "btree/concurrent_traversal.hpp"
#include "btree/get_distribution.hpp"
#include "btree/operations.hpp"
#include "btree/reql_specific.hpp"
#include "btree/operations.hpp"
#include "concurrency/coro_pool.hpp"
#include "concurrency/new_mutex.hpp"
#include "concurrency/queue/unlimited_fifo.hpp"
#include "containers/archive/boost_types.hpp"
#include "containers/archive/buffer_group_stream.hpp"
#include "containers/archive/buffer_stream.hpp"
#include "containers/archive/string_stream.hpp"
#include "containers/scoped.hpp"
#include "rdb_protocol/geo/exceptions.hpp"
#include "rdb_protocol/geo/indexing.hpp"
#include "rdb_protocol/func.hpp"
#include "rdb_protocol/geo_traversal.hpp"
#include "rdb_protocol/pseudo_geometry.hpp"
#include "rdb_protocol/serialize_datum_onto_blob.hpp"
#include "rdb_protocol/shards.hpp"
#include "rdb_protocol/table_common.hpp"

#include "debug.hpp"

void rdb_get(rockshard rocksh, const store_key_t &store_key,
             real_superblock_lock *superblock, point_read_response_t *response) {
    superblock->read_acq_signal()->wait_lazily_ordered();
    std::string loc = rockstore::table_primary_key(rocksh.table_id, rocksh.shard_no, key_to_unescaped_str(store_key));
    std::pair<std::string, bool> val = rocksh.rocks->try_read(loc);
    superblock->reset_superblock();
    if (!val.second) {
        response->data = ql::datum_t::null();
    } else {
        ql::datum_t datum = datum_deserialize_from_vec(val.first.data(), val.first.size());
        response->data = std::move(datum);
    }
}

void kv_location_delete(rockstore::store *rocks,
                        real_superblock_lock *superblock,
                        const std::string &rocks_kv_location,
                        repli_timestamp_t timestamp,
                        delete_mode_t delete_mode) {
    (void)rocks;  // TODO
    (void)timestamp;  // TODO: Use with WAL?
    (void)delete_mode;  // TODO: Use this with WAL?
    superblock->wait_write_batch()->Delete(rocks_kv_location);
}

ql::serialization_result_t datum_serialize_to_string(const ql::datum_t &datum, std::string *out) {
    // TODO: We can avoid double-copying or something, because the write_message_t does
    // get preallocated to one buffer, we can use a rocksdb::Slice.
    write_message_t wm;
    ql::serialization_result_t res =
        datum_serialize(&wm, datum, ql::check_datum_serialization_errors_t::YES);
    if (bad(res)) {
        return res;
    }
    string_stream_t stream;
    int write_res = send_write_message(&stream, &wm);
    guarantee(write_res == 0);
    *out = std::move(stream.str());
    return res;
}


MUST_USE ql::serialization_result_t
kv_location_set(rockstore::store *rocks,
                real_superblock_lock *superblock,
                const std::string &rocks_kv_location,
                ql::datum_t data,
                repli_timestamp_t timestamp) THROWS_NOTHING {
    (void)rocks;  // TODO
    (void)timestamp;  // TODO: Put in WAL

    std::string str;
    ql::serialization_result_t res =
        datum_serialize_to_string(data, &str);
    if (bad(res)) {
        return res;
    }
    superblock->wait_write_batch()->Put(rocks_kv_location, str);

    return ql::serialization_result_t::SUCCESS;
}

// TODO: Does this function really need to exist?
MUST_USE ql::serialization_result_t
kv_location_set_secondary(
        rockstore::store *rocks,
        real_superblock_lock *superblock,
        const std::string &rocks_kv_location,
        const ql::datum_t &value_datum) {
    (void)rocks;  // TODO
    // TODO: Avoid having to reserialize the value again (for each secondary index on top of the primary).
    std::string str;
    ql::serialization_result_t res =
        datum_serialize_to_string(value_datum, &str);
    guarantee(!bad(res));
    superblock->wait_write_batch()->Put(rocks_kv_location, str);
    // TODO: Useless return value;
    return ql::serialization_result_t::SUCCESS;
}

class one_replace_t {
public:
    one_replace_t(const btree_batched_replacer_t *_replacer, size_t _index)
        : replacer(_replacer), index(_index) { }

    ql::datum_t replace(const ql::datum_t &d) const {
        return replacer->replace(d, index);
    }
    return_changes_t should_return_changes() const { return replacer->should_return_changes(); }
private:
    const btree_batched_replacer_t *const replacer;
    const size_t index;
};

batched_replace_response_t rdb_replace_and_return_superblock(
        rockshard rocksh,
        const btree_loc_info_t &info,
        const one_replace_t *replacer,
        promise_t<real_superblock_lock *> *superblock_promise,
        rdb_modification_info_t *mod_info_out,
        profile::trace_t *trace) {
    (void)trace;  // TODO: Use trace?
    const return_changes_t return_changes = replacer->should_return_changes();
    const datum_string_t &primary_key = info.btree->primary_key;
    const store_key_t &key = *info.key;

    try {
        superblock_passback_guard spb(info.superblock, superblock_promise);
        info.btree->slice->stats.pm_keys_set.record();
        info.btree->slice->stats.pm_total_keys_set += 1;

        std::string rocks_kv_location = rockstore::table_primary_key(rocksh.table_id, rocksh.shard_no, key_to_unescaped_str(*info.key));
        std::pair<std::string, bool> maybe_value = rocksh.rocks->try_read(rocks_kv_location);

        ql::datum_t old_val;
        if (!maybe_value.second) {
            // If there's no entry with this key, pass NULL to the function.
            old_val = ql::datum_t::null();
        } else {
            // Otherwise pass the entry with this key to the function.
            old_val = datum_deserialize_from_vec(maybe_value.first.data(), maybe_value.first.size());
            guarantee(old_val.get_field(primary_key, ql::NOTHROW).has());
        }
        guarantee(old_val.has());

        ql::datum_t new_val;
        try {
            /* Compute the replacement value for the row */
            new_val = replacer->replace(old_val);

            /* Validate the replacement value and generate a stats object to return to
            the user, but don't return it yet if we need to make changes. The reason for
            this odd order is that we need to validate the change before we write the
            change. */
            rcheck_row_replacement(primary_key, key, old_val, new_val);
            bool was_changed;
            ql::datum_t resp = make_row_replacement_stats(
                primary_key, key, old_val, new_val, return_changes, &was_changed);
            if (!was_changed) {
                return resp;
            }

            /* Now that the change has passed validation, write it to disk */
            if (new_val.get_type() == ql::datum_t::R_NULL) {
                kv_location_delete(rocksh.rocks, info.superblock, rocks_kv_location,
                                   info.btree->timestamp,
                                   delete_mode_t::REGULAR_QUERY);
            } else {
                r_sanity_check(new_val.get_field(primary_key, ql::NOTHROW).has());
                ql::serialization_result_t res =
                    kv_location_set(rocksh.rocks, info.superblock, rocks_kv_location,
                                    new_val,
                                    info.btree->timestamp);
                if (res & ql::serialization_result_t::ARRAY_TOO_BIG) {
                    rfail_typed_target(&new_val, "Array too large for disk writes "
                                       "(limit 100,000 elements).");
                } else if (res & ql::serialization_result_t::EXTREMA_PRESENT) {
                    rfail_typed_target(&new_val, "`r.minval` and `r.maxval` cannot be "
                                       "written to disk.");
                }
                r_sanity_check(!ql::bad(res));
            }

            /* Report the changes for sindex and change-feed purposes */
            // TODO: Can we just assign R_NULL values to deleted and added?
            if (old_val.get_type() != ql::datum_t::R_NULL) {
                mod_info_out->deleted.first = old_val;
            }
            if (new_val.get_type() != ql::datum_t::R_NULL) {
                mod_info_out->added.first = new_val;
            }

            return resp;

        } catch (const ql::base_exc_t &e) {
            return make_row_replacement_error_stats(old_val,
                                                    new_val,
                                                    return_changes,
                                                    e.what());
        }
    } catch (const interrupted_exc_t &e) {
        ql::datum_object_builder_t object_builder;
        std::string msg = strprintf("interrupted (%s:%d)", __FILE__, __LINE__);
        object_builder.add_error(msg.c_str());
        // We don't rethrow because we're in a coroutine.  Theoretically the
        // above message should never make it back to a user because the calling
        // function will also be interrupted, but we document where it comes
        // from to aid in future debugging if that invariant becomes violated.
        return std::move(object_builder).to_datum();
    }
}

ql::datum_t btree_batched_replacer_t::apply_write_hook(
    const datum_string_t &pkey,
    const ql::datum_t &d,
    const ql::datum_t &res_,
    const ql::datum_t &write_timestamp,
    const counted_t<const ql::func_t> &write_hook) const {
    ql::datum_t res = res_;
    if (write_hook.has()) {
        ql::datum_t primary_key;
        if (res.get_type() != ql::datum_t::type_t::R_NULL) {
            primary_key = res.get_field(pkey, ql::throw_bool_t::NOTHROW);
        } else if (d.get_type() != ql::datum_t::type_t::R_NULL) {
            primary_key = d.get_field(pkey, ql::throw_bool_t::NOTHROW);
        }
        if (!primary_key.has()) {
            primary_key = ql::datum_t::null();
        }
        ql::datum_t modified;
        try {
            cond_t non_interruptor;
            ql::env_t write_hook_env(&non_interruptor,
                                     ql::return_empty_normal_batches_t::NO,
                                     reql_version_t::LATEST);

            ql::datum_object_builder_t builder;
            builder.overwrite("primary_key", std::move(primary_key));
            builder.overwrite("timestamp", write_timestamp);

            modified = write_hook->call(&write_hook_env,
                                        std::vector<ql::datum_t>{
                                            std::move(builder).to_datum(),
                                                d,
                                                res})->as_datum();
        } catch (ql::exc_t &e) {
            throw ql::exc_t(e.get_type(),
                            strprintf("Error in write hook: %s", e.what()),
                            e.backtrace(),
                            e.dummy_frames());
        } catch (ql::datum_exc_t &e) {
            throw ql::datum_exc_t(e.get_type(),
                                  strprintf("Error in write hook: %s", e.what()));
        }

        rcheck_toplevel(!(res.get_type() == ql::datum_t::type_t::R_NULL &&
                          modified.get_type() != ql::datum_t::type_t::R_NULL),
                        ql::base_exc_t::OP_FAILED,
                        "A write hook function must not turn a deletion into a "
                        "replace/insert.");
        rcheck_toplevel(!(res.get_type() != ql::datum_t::type_t::R_NULL &&
                          modified.get_type() == ql::datum_t::type_t::R_NULL),
                        ql::base_exc_t::OP_FAILED,
                        "A write hook function must not turn a replace/insert "
                        "into a deletion.");
        res = modified;
    }
    return res;
}

void do_a_replace_from_batched_replace(
    auto_drainer_t::lock_t,
    rockshard rocksh,
    fifo_enforcer_sink_t *batched_replaces_fifo_sink,
    const fifo_enforcer_write_token_t &batched_replaces_fifo_token,
    btree_loc_info_t &&info,
    const one_replace_t one_replace,
    const ql::configured_limits_t &limits,
    promise_t<real_superblock_lock *> *superblock_promise,
    rdb_modification_report_cb_t *mod_cb,
    bool update_pkey_cfeeds,
    batched_replace_response_t *stats_out,
    profile::trace_t *trace,
    std::set<std::string> *conditions) {

    fifo_enforcer_sink_t::exit_write_t exiter(
        batched_replaces_fifo_sink, batched_replaces_fifo_token);
    // We need to get in line for this while still holding the superblock so
    // that stamp read operations can't queue-skip.
    info.superblock->write_acq_signal()->wait_lazily_ordered();
    rwlock_in_line_t stamp_spot = mod_cb->get_in_line_for_cfeed_stamp();

    rdb_modification_report_t mod_report(*info.key);
    ql::datum_t res = rdb_replace_and_return_superblock(
        rocksh, std::move(info), &one_replace, superblock_promise, &mod_report.info,
        trace);
    *stats_out = (*stats_out).merge(res, ql::stats_merge, limits, conditions);

    // We wait to make sure we acquire `acq` in the same order we were
    // originally called.
    exiter.wait();
    new_mutex_in_line_t sindex_spot = mod_cb->get_in_line_for_sindex();

    mod_cb->on_mod_report(
        rocksh, mod_report, update_pkey_cfeeds, &sindex_spot, &stamp_spot);
}

batched_replace_response_t rdb_batched_replace(
    rockshard rocksh,
    const btree_info_t &info,
    real_superblock_lock *superblock,
    const std::vector<store_key_t> &keys,
    const btree_batched_replacer_t *replacer,
    rdb_modification_report_cb_t *sindex_cb,
    ql::configured_limits_t limits,
    profile::sampler_t *sampler,
    profile::trace_t *trace) {

    fifo_enforcer_source_t source;
    fifo_enforcer_sink_t sink;

    ql::datum_t stats = ql::datum_t::empty_object();

    std::set<std::string> conditions;

    // We have to drain write operations before destructing everything above us,
    // because the coroutines being drained use them.
    {
        // We must disable profiler events for subtasks, because multiple instances
        // of `handle_pair`are going to run in parallel which  would otherwise corrupt
        // the sequence of events in the profiler trace.
        // Instead we add a single event for the whole batched replace.
        sampler->new_sample();
        PROFILE_STARTER_IF_ENABLED(
            trace != nullptr,
            "Perform parallel replaces.",
            trace);
        profile::disabler_t trace_disabler(trace);
        unlimited_fifo_queue_t<std::function<void()> > coro_queue;
        struct callback_t : public coro_pool_callback_t<std::function<void()> > {
            virtual void coro_pool_callback(std::function<void()> f, signal_t *) {
                f();
            }
        } callback;
        const size_t MAX_CONCURRENT_REPLACES = 8;
        coro_pool_t<std::function<void()> > coro_pool(
            MAX_CONCURRENT_REPLACES, &coro_queue, &callback);
        // We do not release the superblock. This comment was before the rocks replacement.
        // TODO: Should we release... something... early?  Where?
        // We release the superblock either before or after draining on all the
        // write operations depending on the presence of limit changefeeds.
        // TODO: Okay, continue
        bool update_pkey_cfeeds = sindex_cb->has_pkey_cfeeds(keys);
        {
            auto_drainer_t drainer;
            for (size_t i = 0; i < keys.size(); ++i) {
                promise_t<real_superblock_lock *> superblock_promise;
                coro_queue.push(
                    [lock = auto_drainer_t::lock_t(&drainer),
                     rocksh,
                     &sink,
                     source_token = source.enter_write(),
                     info = btree_loc_info_t(&info, superblock, &keys[i]),
                     one_replace = one_replace_t(replacer, i),
                     limits,
                     &superblock_promise,
                     sindex_cb,
                     update_pkey_cfeeds,
                     &stats,
                     trace,
                     &conditions]() mutable {
                        do_a_replace_from_batched_replace(
                            lock,
                            rocksh,
                            &sink,
                            source_token,
                            std::move(info),
                            one_replace,
                            limits,
                            &superblock_promise,
                            sindex_cb,
                            update_pkey_cfeeds,
                            &stats,
                            trace,
                            &conditions);
                     });
                superblock_promise.wait();
            }
            if (!update_pkey_cfeeds) {
                // TODO: We don't do this anymore.  (Should we?)
                // current_superblock.reset(); // Release the superblock early if
                //                             // we don't need to finish.
            }
        }
        // This needs to happen after draining.
        if (update_pkey_cfeeds) {
            sindex_cb->finish(info.slice, superblock);
        }
    }

    ql::datum_object_builder_t out(stats);
    out.add_warnings(conditions, limits);
    return std::move(out).to_datum();
}

void rdb_set(rockshard rocksh,
             const store_key_t &key,
             ql::datum_t data,
             bool overwrite,  /* Right now, via point_write_t::overwrite this is always true. */
             btree_slice_t *slice,
             repli_timestamp_t timestamp,
             real_superblock_lock *superblock,
             point_write_response_t *response_out,
             rdb_modification_info_t *mod_info,
             profile::trace_t *trace,
             promise_t<real_superblock_lock *> *pass_back_superblock) {
    (void)trace;  // TODO: Use trace?
    superblock_passback_guard spb(superblock, pass_back_superblock);
    slice->stats.pm_keys_set.record();
    slice->stats.pm_total_keys_set += 1;

    superblock->read_acq_signal()->wait_lazily_ordered();

    std::string rocks_kv_location = rockstore::table_primary_key(rocksh.table_id, rocksh.shard_no, key_to_unescaped_str(key));
    std::pair<std::string, bool> maybe_value
        = rocksh.rocks->try_read(rocks_kv_location);

    const bool had_value = maybe_value.second;

    /* update the modification report */
    if (maybe_value.second) {
        ql::datum_t val = datum_deserialize_from_vec(maybe_value.first.data(), maybe_value.first.size());
        mod_info->deleted.first = val;
    }

    mod_info->added.first = data;

    if (overwrite || !had_value) {
        superblock->write_acq_signal()->wait_lazily_ordered();
        // TODO: We don't do kv_location_delete in case of R_NULL value?
        // (I think the value can't be R_NULL because backfilling code is what calls this.)
        ql::serialization_result_t res =
            kv_location_set(rocksh.rocks, superblock, rocks_kv_location,
                            data, timestamp);
        if (res & ql::serialization_result_t::ARRAY_TOO_BIG) {
            rfail_typed_target(&data, "Array too large for disk writes "
                               "(limit 100,000 elements).");
        } else if (res & ql::serialization_result_t::EXTREMA_PRESENT) {
            rfail_typed_target(&data, "`r.minval` and `r.maxval` cannot be "
                               "written to disk.");
        }
        r_sanity_check(!ql::bad(res));
    }
    response_out->result =
        (had_value ? point_write_result_t::DUPLICATE : point_write_result_t::STORED);
}


void rdb_delete(rockshard rocksh,
                const store_key_t &key,
                btree_slice_t *slice,
                repli_timestamp_t timestamp,
                real_superblock_lock *superblock,
                delete_mode_t delete_mode,
                point_delete_response_t *response,
                rdb_modification_info_t *mod_info,
                profile::trace_t *trace,
                promise_t<real_superblock_lock *> *pass_back_superblock) {
    (void)trace;  // TODO: Use trace?
    superblock_passback_guard spb(superblock, pass_back_superblock);
    slice->stats.pm_keys_set.record();
    slice->stats.pm_total_keys_set += 1;

    superblock->read_acq_signal()->wait_lazily_ordered();

    std::string rocks_kv_location = rockstore::table_primary_key(rocksh.table_id, rocksh.shard_no, key_to_unescaped_str(key));
    std::pair<std::string, bool> maybe_value = rocksh.rocks->try_read(rocks_kv_location);

    bool exists = maybe_value.second;

    /* Update the modification report. */
    if (exists) {
        superblock->write_acq_signal()->wait_lazily_ordered();
        mod_info->deleted.first = datum_deserialize_from_vec(maybe_value.first.data(), maybe_value.first.size());
        kv_location_delete(rocksh.rocks, superblock, rocks_kv_location, timestamp, delete_mode);
    }
    response->result = (exists ? point_delete_result_t::DELETED : point_delete_result_t::MISSING);
}

typedef ql::transform_variant_t transform_variant_t;
typedef ql::terminal_variant_t terminal_variant_t;

class rget_sindex_data_t {
public:
    rget_sindex_data_t(key_range_t _pkey_range,
                       ql::datumspec_t _datumspec,
                       key_range_t *_active_region_range_inout,
                       reql_version_t wire_func_reql_version,
                       ql::map_wire_func_t wire_func,
                       sindex_multi_bool_t _multi)
        : pkey_range(std::move(_pkey_range)),
          datumspec(std::move(_datumspec)),
          active_region_range_inout(_active_region_range_inout),
          func_reql_version(wire_func_reql_version),
          func(wire_func.compile_wire_func()),
          multi(_multi) {
        datumspec.visit<void>(
            [&](const ql::datum_range_t &r) {
                lbound_trunc_key = r.get_left_bound_trunc_key(func_reql_version);
                rbound_trunc_key = r.get_right_bound_trunc_key(func_reql_version);
            },
            [](const std::map<ql::datum_t, uint64_t> &) { });
    }
private:
    friend class rocks_rget_secondary_cb;
    const key_range_t pkey_range;
    const ql::datumspec_t datumspec;
    key_range_t *active_region_range_inout;
    const reql_version_t func_reql_version;
    const counted_t<const ql::func_t> func;
    const sindex_multi_bool_t multi;
    // The (truncated) boundary keys for the datum range stored in `datumspec`.
    std::string lbound_trunc_key;
    std::string rbound_trunc_key;
};

class job_data_t {
public:
    job_data_t(ql::env_t *_env,
               const ql::batchspec_t &batchspec,
               const std::vector<transform_variant_t> &_transforms,
               const optional<terminal_variant_t> &_terminal,
               region_t region,
               store_key_t last_key,
               sorting_t _sorting,
               require_sindexes_t require_sindex_val)
        : env(_env),
          batcher(make_scoped<ql::batcher_t>(batchspec.to_batcher())),
          sorting(_sorting),
          accumulator(_terminal.has_value()
                      ? ql::make_terminal(*_terminal)
                      : ql::make_append(std::move(region),
                                        std::move(last_key),
                                        sorting,
                                        batcher.get(),
                                        require_sindex_val)) {
        for (size_t i = 0; i < _transforms.size(); ++i) {
            transformers.push_back(ql::make_op(_transforms[i]));
        }
        guarantee(transformers.size() == _transforms.size());
    }
    job_data_t(job_data_t &&) = default;

    bool should_send_batch() const {
        return accumulator->should_send_batch();
    }
private:
    friend class rocks_rget_cb;
    friend class rocks_rget_secondary_cb;
    ql::env_t *const env;
    scoped_ptr_t<ql::batcher_t> batcher;
    std::vector<scoped_ptr_t<ql::op_t> > transformers;
    sorting_t sorting;
    scoped_ptr_t<ql::accumulator_t> accumulator;
};

class rget_io_data_t {
public:
    rget_io_data_t(rget_read_response_t *_response, btree_slice_t *_slice)
        : response(_response), slice(_slice) { }
private:
    friend class rocks_rget_cb;
    friend class rocks_rget_secondary_cb;
    rget_read_response_t *const response;
    btree_slice_t *const slice;
};

class rocks_rget_cb {
public:
    rocks_rget_cb(
        rget_io_data_t &&_io,
        job_data_t &&_job);

    continue_bool_t handle_pair(
        std::pair<const char *, size_t> key, std::pair<const char *, size_t> value,
        size_t default_copies)
        THROWS_ONLY(interrupted_exc_t);
    void finish(continue_bool_t last_cb) THROWS_ONLY(interrupted_exc_t);
private:
    const rget_io_data_t io; // How do get data in/out.
    job_data_t job; // What to do next (stateful).

    // State for internal bookkeeping.
    bool bad_init;
    scoped_ptr_t<profile::disabler_t> disabler;
    scoped_ptr_t<profile::sampler_t> sampler;
};

// This is the interface the btree code expects, but our actual callback needs a
// little bit more so we use this wrapper to hold the extra information.
class rocks_rget_cb_wrapper : public rocks_traversal_cb {
public:
    rocks_rget_cb_wrapper(
            rocks_rget_cb *_cb,
            size_t _copies)
        : cb(_cb), copies(_copies) { }
    virtual continue_bool_t handle_pair(
        std::pair<const char *, size_t> key, std::pair<const char *, size_t> value)
        THROWS_ONLY(interrupted_exc_t) {
        return cb->handle_pair(
            key, value,
            copies);
    }
private:
    rocks_rget_cb *cb;
    size_t copies;
};

rocks_rget_cb::rocks_rget_cb(rget_io_data_t &&_io,
        job_data_t &&_job)
    : io(std::move(_io)),
      job(std::move(_job)),
      bad_init(false) {

    // We must disable profiler events for subtasks, because multiple instances
    // of `handle_pair`are going to run in parallel which  would otherwise corrupt
    // the sequence of events in the profiler trace.
    disabler.init(new profile::disabler_t(job.env->trace));
    sampler.init(new profile::sampler_t("Range traversal doc evaluation.",
                                        job.env->trace));
}

void rocks_rget_cb::finish(continue_bool_t last_cb) THROWS_ONLY(interrupted_exc_t) {
    job.accumulator->finish(last_cb, &io.response->result);
}

// Handle a keyvalue pair.  Returns whether or not we're done early.
continue_bool_t rocks_rget_cb::handle_pair(
        std::pair<const char *, size_t> keyslice, std::pair<const char *, size_t> value,
        size_t default_copies)
    THROWS_ONLY(interrupted_exc_t) {

    //////////////////////////////////////////////////
    // STUFF THAT CAN HAPPEN OUT OF ORDER GOES HERE //
    //////////////////////////////////////////////////
    sampler->new_sample();
    if (bad_init || boost::get<ql::exc_t>(&io.response->result) != nullptr) {
        return continue_bool_t::ABORT;
    }
    // Load the key and value.
    store_key_t key(keyslice.second, reinterpret_cast<const uint8_t *>(keyslice.first));
    ql::datum_t val;
    // Count stats whether or not we deserialize the value
    io.slice->stats.pm_keys_read.record();
    io.slice->stats.pm_total_keys_read += 1;
    // We only load the value if we actually use it (`count` does not).
    if (job.accumulator->uses_val() || job.transformers.size() != 0) {
        val = datum_deserialize_from_vec(value.first, value.second);
    }

    // Note: Previously (before converting to use with rocksdb, we had a
    // concurrent_traversal operation that spawned this method in multiple
    // coroutines.  They'd get an order token and then we'd realign with the
    // waiter right here, and proceed to process key/value pairs in order below.)

    // TODO: We could do the rest of this rocks_rget_cb::handle_pair function concurrently.

    // TODO: Can this function still throw interrupted_exc_t?

    ///////////////////////////////////////////////////////
    // STUFF THAT HAS TO HAPPEN IN ORDER GOES BELOW HERE //
    ///////////////////////////////////////////////////////

    try {
        auto lazy_sindex_val = []() -> ql::datum_t {
            return ql::datum_t();
        };

        // Check whether we're outside the sindex range.
        // We only need to check this if we are on the boundary of the sindex range, and
        // the involved keys are truncated.
        size_t copies = default_copies;

        ql::groups_t data = {{ql::datum_t(), ql::datums_t(copies, val)}};

        for (auto it = job.transformers.begin(); it != job.transformers.end(); ++it) {
            (**it)(job.env, &data, lazy_sindex_val);
        }
        // We need lots of extra data for the accumulation because we might be
        // accumulating `rget_item_t`s for a batch.
        continue_bool_t cont = (*job.accumulator)(job.env, &data, key, lazy_sindex_val);
        return cont;
    } catch (const ql::exc_t &e) {
        io.response->result = e;
        return continue_bool_t::ABORT;
    } catch (const ql::datum_exc_t &e) {
#ifndef NDEBUG
        unreachable();
#else
        io.response->result = ql::exc_t(e, ql::backtrace_id_t::empty());
        return continue_bool_t::ABORT;
#endif // NDEBUG
    }
}


class rocks_rget_secondary_cb {
public:
    rocks_rget_secondary_cb(
        rget_io_data_t &&_io,
        job_data_t &&_job,
        rget_sindex_data_t &&_sindex_data);

    continue_bool_t handle_pair(
        std::pair<const char *, size_t> key, std::pair<const char *, size_t> value,
        size_t default_copies,
        const std::string &skey_left)
        THROWS_ONLY(interrupted_exc_t);
    void finish(continue_bool_t last_cb) THROWS_ONLY(interrupted_exc_t);
private:
    const rget_io_data_t io; // How do get data in/out.
    job_data_t job; // What to do next (stateful).
    const rget_sindex_data_t sindex_data; // Optional sindex information.

    scoped_ptr_t<ql::env_t> sindex_env;

    // State for internal bookkeeping.
    bool bad_init;
    optional<std::string> last_truncated_secondary_for_abort;
    scoped_ptr_t<profile::disabler_t> disabler;
    scoped_ptr_t<profile::sampler_t> sampler;
};

// This is the interface the btree code expects, but our actual callback needs a
// little bit more so we use this wrapper to hold the extra information.
class rocks_rget_secondary_cb_wrapper : public rocks_traversal_cb {
public:
    rocks_rget_secondary_cb_wrapper(
            rocks_rget_secondary_cb *_cb,
            size_t _copies,
            std::string _skey_left)
        : cb(_cb), copies(_copies), skey_left(std::move(_skey_left)) { }
    virtual continue_bool_t handle_pair(
        std::pair<const char *, size_t> key, std::pair<const char *, size_t> value)
        THROWS_ONLY(interrupted_exc_t) {
        return cb->handle_pair(
            key, value,
            copies,
            skey_left);
    }
private:
    rocks_rget_secondary_cb *cb;
    size_t copies;
    std::string skey_left;
};

rocks_rget_secondary_cb::rocks_rget_secondary_cb(rget_io_data_t &&_io,
        job_data_t &&_job,
        rget_sindex_data_t &&_sindex_data)
    : io(std::move(_io)),
      job(std::move(_job)),
      sindex_data(std::move(_sindex_data)),
      bad_init(false) {

    // Secondary index functions are deterministic (so no need for an
    // rdb_context_t) and evaluated in a pristine environment (without global
    // optargs).
    sindex_env.init(new ql::env_t(job.env->interruptor,
                                  ql::return_empty_normal_batches_t::NO,
                                  sindex_data.func_reql_version));

    // We must disable profiler events for subtasks, because multiple instances
    // of `handle_pair`are going to run in parallel which  would otherwise corrupt
    // the sequence of events in the profiler trace.
    disabler.init(new profile::disabler_t(job.env->trace));
    sampler.init(new profile::sampler_t("Range traversal doc evaluation.",
                                        job.env->trace));
}

void rocks_rget_secondary_cb::finish(continue_bool_t last_cb) THROWS_ONLY(interrupted_exc_t) {
    job.accumulator->finish(last_cb, &io.response->result);
}

// Handle a keyvalue pair.  Returns whether or not we're done early.
continue_bool_t rocks_rget_secondary_cb::handle_pair(
        std::pair<const char *, size_t> keyslice, std::pair<const char *, size_t> value,
        size_t default_copies,
        const std::string &skey_left)
    THROWS_ONLY(interrupted_exc_t) {

    //////////////////////////////////////////////////
    // STUFF THAT CAN HAPPEN OUT OF ORDER GOES HERE //
    //////////////////////////////////////////////////
    sampler->new_sample();
    if (bad_init || boost::get<ql::exc_t>(&io.response->result) != nullptr) {
        return continue_bool_t::ABORT;
    }
    // Load the key and value.
    store_key_t key(keyslice.second, reinterpret_cast<const uint8_t *>(keyslice.first));
    if (!sindex_data.pkey_range.contains_key(ql::datum_t::extract_primary(key))) {
        return continue_bool_t::CONTINUE;
    }
    ql::datum_t val;
    // Count stats whether or not we deserialize the value
    io.slice->stats.pm_keys_read.record();
    io.slice->stats.pm_total_keys_read += 1;
    // We always load the value because secondary index uses it...
    val = datum_deserialize_from_vec(value.first, value.second);

    // Note: Previously (before converting to use with rocksdb, we had a
    // concurrent_traversal operation that spawned this method in multiple
    // coroutines.  They'd get an order token and then we'd realign with the
    // waiter right here, and proceed to process key/value pairs in order below.)

    // TODO: We could do the rest of this rocks_rget_secondary_cb::handle_pair function concurrently.

    // TODO: Can this function still throw interrupted_exc_t?

    ///////////////////////////////////////////////////////
    // STUFF THAT HAS TO HAPPEN IN ORDER GOES BELOW HERE //
    ///////////////////////////////////////////////////////

    // If the sindex portion of the key is long enough that it might be >= the
    // length of a truncated sindex, we need to rember the key so we can make
    // sure not to stop in the middle of a sindex range where some of the values
    // are out of order because of truncation.
    bool remember_key_for_sindex_batching =
        (ql::datum_t::extract_secondary(key_to_unescaped_str(key)).size()
           >= ql::datum_t::max_trunc_size());
    if (last_truncated_secondary_for_abort) {
        std::string cur_truncated_secondary =
            ql::datum_t::extract_truncated_secondary(key_to_unescaped_str(key));
        if (cur_truncated_secondary != *last_truncated_secondary_for_abort) {
            // The semantics here are that we're returning the "last considered
            // key", which we set specially here to preserve the invariant that
            // unsharding either consumes all rows with a particular truncated
            // sindex value or none of them.
            store_key_t stop_key;
            if (!reversed(job.sorting)) {
                stop_key = store_key_t(cur_truncated_secondary);
            } else {
                stop_key = store_key_t(*last_truncated_secondary_for_abort);
            }
            stop_key.decrement();
            job.accumulator->stop_at_boundary(std::move(stop_key));
            return continue_bool_t::ABORT;
        }
    }

    try {
        // Update the active region range.
        if (!reversed(job.sorting)) {
            if (sindex_data.active_region_range_inout->left < key) {
                sindex_data.active_region_range_inout->left = key;
                sindex_data.active_region_range_inout->left.increment();
            }
        } else {
            if (key < sindex_data.active_region_range_inout->right.key_or_max()) {
                sindex_data.active_region_range_inout->right =
                    key_range_t::right_bound_t(key);
            }
        }

        // There are certain transformations and accumulators that need the
        // secondary index value, though many don't. We don't want to compute
        // it if we don't end up needing it, because that would be expensive.
        // So we provide a function that computes the secondary index value
        // lazily the first time it's called.
        ql::datum_t sindex_val_cache; // an empty `datum_t` until initialized
        auto lazy_sindex_val = [&]() -> ql::datum_t {
            if (!sindex_val_cache.has()) {
                sindex_val_cache =
                    sindex_data.func->call(sindex_env.get(), val)->as_datum();
                if (sindex_data.multi == sindex_multi_bool_t::MULTI
                    && sindex_val_cache.get_type() == ql::datum_t::R_ARRAY) {
                    uint64_t tag = ql::datum_t::extract_tag(key).get();
                    sindex_val_cache = sindex_val_cache.get(tag, ql::NOTHROW);
                    guarantee(sindex_val_cache.has());
                }
            }
            return sindex_val_cache;
        };

        // Check whether we're outside the sindex range.
        // We only need to check this if we are on the boundary of the sindex range, and
        // the involved keys are truncated.
        size_t copies = default_copies;
        if (true) {
            /* Here's an attempt at explaining the different case distinctions handled in
               this check (for the left bound; the right bound check is similar):
               The case distinctions are as follows:
               1. left_bound_is_truncated
                If the left bound key had to be truncated, we first compare the prefix of
                the current secondary key (skey_current), and the left bound key.
                The comparison cannot be -1, because that would mean that we computed the
                traversal key range incorrectly in the first place (there's no need to
                consider keys that are *smaller* than the left bound).
                If the comparison is 1, the current key's secondary part is larger than
                the left bound, and we know that the corresponding datum_t value must
                also be larger than the datum_t corresponding to the left bound.
                Finally, since the left bound is truncated, the comparison can determine
                that the prefix is equal for values in the btree with corresponding index
                values that are either left of the bound (but match in the truncated
                prefix), at the bound (which we want to include only if the left bound is
                closed), or right of the bound (which we always want to include, as far
                as the left bound id concerned). We can't determine which case we have,
                by looking only at the keys. Hence we must check the number of copies for
                `cmp == 0`. The only exception is if the current key was actually not
                truncated, in which case we know that it will actually be smaller than
                the left bound (that's encoded in line 825).
               2. !left_bound_is_truncated && left_bound is closed
                If the bound wasn't truncated, we know that the traversal range will not
                include any values which are smaller than the left bound. Hence we can
                skip the check for whether the sindex value is actually in the datum
                range.
               3. !left_bound_is_truncated && left_bound is open
                In contrast, if the left bound is open, we compare the left bound and
                current key. If they have the same size and their contents compare equal,
                we actually know that they are outside the range and could set the number
                of copies to 0. We do the slightly less optimal but simpler thing and
                just check the number of copies in this case, so that we can share the
                code path with case 1. */
            const size_t max_trunc_size = ql::datum_t::max_trunc_size();
            sindex_data.datumspec.visit<void>(
            [&](const ql::datum_range_t &r) {
                bool must_check_copies = false;
                std::string skey_current =
                    ql::datum_t::extract_truncated_secondary(key_to_unescaped_str(key));
                const bool left_bound_is_truncated =
                    sindex_data.lbound_trunc_key.size() == max_trunc_size;
                if (left_bound_is_truncated
                    || r.left_bound_type == key_range_t::bound_t::open) {
                    int cmp = memcmp(
                        skey_current.data(),
                        sindex_data.lbound_trunc_key.data(),
                        std::min<size_t>(skey_current.size(),
                                         sindex_data.lbound_trunc_key.size()));
                    if (skey_current.size() < sindex_data.lbound_trunc_key.size()) {
                        guarantee(cmp != 0);
                    }
                    guarantee(cmp >= 0);
                    if (cmp == 0
                        && skey_current.size() == sindex_data.lbound_trunc_key.size()) {
                        must_check_copies = true;
                    }
                }
                if (!must_check_copies) {
                    const bool right_bound_is_truncated =
                        sindex_data.rbound_trunc_key.size() == max_trunc_size;
                    if (right_bound_is_truncated
                        || r.right_bound_type == key_range_t::bound_t::open) {
                        int cmp = memcmp(
                            skey_current.data(),
                            sindex_data.rbound_trunc_key.data(),
                            std::min<size_t>(skey_current.size(),
                                             sindex_data.rbound_trunc_key.size()));
                        if (skey_current.size() > sindex_data.rbound_trunc_key.size()) {
                            guarantee(cmp != 0);
                        }
                        guarantee(cmp <= 0);
                        if (cmp == 0
                            && skey_current.size() == sindex_data.rbound_trunc_key.size()) {
                            must_check_copies = true;
                        }
                    }
                }
                if (must_check_copies) {
                    copies = sindex_data.datumspec.copies(lazy_sindex_val());
                } else {
                    copies = 1;
                }
            },
            [&](const std::map<ql::datum_t, uint64_t> &) {
                std::string skey_current =
                    ql::datum_t::extract_secondary(key_to_unescaped_str(key));
                const bool skey_current_is_truncated =
                    skey_current.size() >= max_trunc_size;
                const bool skey_left_is_truncated = skey_left.size() >= max_trunc_size;

                if (skey_current_is_truncated || skey_left_is_truncated) {
                    copies = sindex_data.datumspec.copies(lazy_sindex_val());
                } else if (skey_left != skey_current) {
                    copies = 0;
                }
            });
            if (copies == 0) {
                return continue_bool_t::CONTINUE;
            }
        }

        ql::groups_t data = {{ql::datum_t(), ql::datums_t(copies, val)}};

        for (auto it = job.transformers.begin(); it != job.transformers.end(); ++it) {
            (**it)(job.env, &data, lazy_sindex_val);
        }
        // We need lots of extra data for the accumulation because we might be
        // accumulating `rget_item_t`s for a batch.
        continue_bool_t cont = (*job.accumulator)(job.env, &data, key, lazy_sindex_val);
        if (remember_key_for_sindex_batching) {
            if (cont == continue_bool_t::ABORT) {
                last_truncated_secondary_for_abort.set(
                    ql::datum_t::extract_truncated_secondary(key_to_unescaped_str(key)));
            }
            return continue_bool_t::CONTINUE;
        } else {
            return cont;
        }
    } catch (const ql::exc_t &e) {
        io.response->result = e;
        return continue_bool_t::ABORT;
    } catch (const ql::datum_exc_t &e) {
#ifndef NDEBUG
        unreachable();
#else
        io.response->result = ql::exc_t(e, ql::backtrace_id_t::empty());
        return continue_bool_t::ABORT;
#endif // NDEBUG
    }
}

// TODO: Remove/merge with rdb_rget_slice again?

// TODO: Flip snap/rocksh argument order, to be consistent.
void rdb_rget_snapshot_slice(
        const rocksdb::Snapshot *snap,
        rockshard rocksh,
        btree_slice_t *slice,
        const region_t &shard,
        const key_range_t &range,
        const optional<std::map<store_key_t, uint64_t> > &primary_keys,
        ql::env_t *ql_env,
        const ql::batchspec_t &batchspec,
        const std::vector<transform_variant_t> &transforms,
        const optional<terminal_variant_t> &terminal,
        sorting_t sorting,
        rget_read_response_t *response) {
    r_sanity_check(boost::get<ql::exc_t>(&response->result) == nullptr);
    PROFILE_STARTER_IF_ENABLED(
        ql_env->profile() == profile_bool_t::PROFILE,
        "Do range scan on primary index.",
        ql_env->trace);

    rocks_rget_cb callback(
        rget_io_data_t(response, slice),
        job_data_t(ql_env,
                   batchspec,
                   transforms,
                   terminal,
                   shard,
                   !reversed(sorting)
                       ? range.left
                       : range.right.key_or_max(),
                   sorting,
                   require_sindexes_t::NO));

    direction_t direction = reversed(sorting) ? direction_t::backward : direction_t::forward;
    continue_bool_t cont = continue_bool_t::CONTINUE;
    std::string rocks_kv_prefix = rockstore::table_primary_prefix(rocksh.table_id, rocksh.shard_no);
    if (primary_keys.has_value()) {
        // TODO: Instead of holding onto the superblock, we could make an iterator once,
        // or hold a rocksdb snapshot once, out here.
        auto cb = [&](const std::pair<store_key_t, uint64_t> &pair) {
            rocks_rget_cb_wrapper wrapper(&callback, pair.second);
            return rocks_traversal(
                rocksh.rocks,
                snap,
                rocks_kv_prefix,
                key_range_t::one_key(pair.first),
                direction,
                &wrapper);
        };
        if (!reversed(sorting)) {
            for (auto it = primary_keys->begin(); it != primary_keys->end();) {
                auto this_it = it++;
                if (cb(*this_it) == continue_bool_t::ABORT) {
                    // If required the superblock will get released further up the stack.
                    cont = continue_bool_t::ABORT;
                    break;
                }
            }
        } else {
            for (auto it = primary_keys->rbegin(); it != primary_keys->rend();) {
                auto this_it = it++;
                if (cb(*this_it) == continue_bool_t::ABORT) {
                    // If required the superblock will get released further up the stack.
                    cont = continue_bool_t::ABORT;
                    break;
                }
            }
        }
    } else {
        rocks_rget_cb_wrapper wrapper(&callback, 1);
        cont = rocks_traversal(
            rocksh.rocks, snap, rocks_kv_prefix, range, direction, &wrapper);
    }
    callback.finish(cont);
}


// TODO: Having two functions which are 99% the same sucks.
void rdb_rget_slice(
        rockshard rocksh,
        btree_slice_t *slice,
        const region_t &shard,
        const key_range_t &range,
        const optional<std::map<store_key_t, uint64_t> > &primary_keys,
        real_superblock_lock *superblock,
        ql::env_t *ql_env,
        const ql::batchspec_t &batchspec,
        const std::vector<transform_variant_t> &transforms,
        const optional<terminal_variant_t> &terminal,
        sorting_t sorting,
        rget_read_response_t *response,
        release_superblock_t release_superblock) {
    r_sanity_check(boost::get<ql::exc_t>(&response->result) == nullptr);
    PROFILE_STARTER_IF_ENABLED(
        ql_env->profile() == profile_bool_t::PROFILE,
        "Do range scan on primary index.",
        ql_env->trace);

    rocks_rget_cb callback(
        rget_io_data_t(response, slice),
        job_data_t(ql_env,
                   batchspec,
                   transforms,
                   terminal,
                   shard,
                   !reversed(sorting)
                       ? range.left
                       : range.right.key_or_max(),
                   sorting,
                   require_sindexes_t::NO));

    direction_t direction = reversed(sorting) ? direction_t::backward : direction_t::forward;
    continue_bool_t cont = continue_bool_t::CONTINUE;
    std::string rocks_kv_prefix = rockstore::table_primary_prefix(rocksh.table_id, rocksh.shard_no);
    if (primary_keys.has_value()) {
        // TODO: Instead of holding onto the superblock, we could make an iterator once,
        // or hold a rocksdb snapshot once, out here.
        auto cb = [&](const std::pair<store_key_t, uint64_t> &pair, bool is_last) {
            rocks_rget_cb_wrapper wrapper(&callback, pair.second);
            // Acquire read lock on superblock and make a snapshot.
            superblock->read_acq_signal()->wait_lazily_ordered();
            rockstore::snapshot snap = make_snapshot(rocksh.rocks);
            if (is_last && release_superblock == release_superblock_t::RELEASE) {
                superblock->reset_superblock();
            }

            return rocks_traversal(
                rocksh.rocks,
                snap.snap,
                rocks_kv_prefix,
                key_range_t::one_key(pair.first),
                direction,
                &wrapper);
        };
        if (!reversed(sorting)) {
            for (auto it = primary_keys->begin(); it != primary_keys->end();) {
                auto this_it = it++;
                if (cb(*this_it, it == primary_keys->end()) == continue_bool_t::ABORT) {
                    // If required the superblock will get released further up the stack.
                    cont = continue_bool_t::ABORT;
                    break;
                }
            }
        } else {
            for (auto it = primary_keys->rbegin(); it != primary_keys->rend();) {
                auto this_it = it++;
                if (cb(*this_it, it == primary_keys->rend()) == continue_bool_t::ABORT) {
                    // If required the superblock will get released further up the stack.
                    cont = continue_bool_t::ABORT;
                    break;
                }
            }
        }
    } else {
        rocks_rget_cb_wrapper wrapper(&callback, 1);
        superblock->read_acq_signal()->wait_lazily_ordered();
        rockstore::snapshot snap = make_snapshot(rocksh.rocks);
        if (release_superblock == release_superblock_t::RELEASE) {
            superblock->reset_superblock();
        }
        cont = rocks_traversal(
            rocksh.rocks, snap.snap, rocks_kv_prefix, range, direction, &wrapper);
    }
    callback.finish(cont);
}


void rdb_rget_secondary_snapshot_slice(
        const rocksdb::Snapshot *snap,
        rockshard rocksh,
        uuid_u sindex_uuid,
        btree_slice_t *slice,
        const region_t &shard,
        const ql::datumspec_t &datumspec,
        const key_range_t &sindex_region_range,
        ql::env_t *ql_env,
        const ql::batchspec_t &batchspec,
        const std::vector<transform_variant_t> &transforms,
        const optional<terminal_variant_t> &terminal,
        const key_range_t &pk_range,
        sorting_t sorting,
        require_sindexes_t require_sindex_val,
        const sindex_disk_info_t &sindex_info,
        rget_read_response_t *response) {
    r_sanity_check(boost::get<ql::exc_t>(&response->result) == nullptr);
    guarantee(sindex_info.geo == sindex_geo_bool_t::REGULAR);
    PROFILE_STARTER_IF_ENABLED(
        ql_env->profile() == profile_bool_t::PROFILE,
        "Do range scan on secondary index.",
        ql_env->trace);

    const reql_version_t sindex_func_reql_version =
        sindex_info.mapping_version_info.latest_compatible_reql_version;

    key_range_t active_region_range = sindex_region_range;
    rocks_rget_secondary_cb callback(
        rget_io_data_t(response, slice),
        job_data_t(ql_env,
                   batchspec,
                   transforms,
                   terminal,
                   shard,
                   !reversed(sorting)
                       ? sindex_region_range.left
                       : sindex_region_range.right.key_or_max(),
                   sorting,
                   require_sindex_val),
        rget_sindex_data_t(
            pk_range,
            datumspec,
            &active_region_range,
            sindex_func_reql_version,
            sindex_info.mapping,
            sindex_info.multi));

    std::string rocks_kv_prefix = rockstore::table_secondary_prefix(rocksh.table_id, rocksh.shard_no, sindex_uuid);

    direction_t direction = reversed(sorting) ? direction_t::backward : direction_t::forward;
    auto cb = [&](const std::pair<ql::datum_range_t, uint64_t> &pair, UNUSED bool is_last) {
        key_range_t sindex_keyrange =
            pair.first.to_sindex_keyrange(sindex_func_reql_version);
        rocks_rget_secondary_cb_wrapper wrapper(
            &callback,
            pair.second,
            key_to_unescaped_str(sindex_keyrange.left));
        key_range_t active_range = active_region_range.intersection(sindex_keyrange);
        // This can happen sometimes with truncated keys.
        if (active_range.is_empty()) return continue_bool_t::CONTINUE;
        return rocks_traversal(
            rocksh.rocks,
            snap,
            rocks_kv_prefix,
            active_range,
            direction,
            &wrapper);
    };
    continue_bool_t cont = datumspec.iter(sorting, cb);
    // TODO: See if anybody else calls datumspec.iter, can we remove is_last parameter.
    callback.finish(cont);
}


// TODO: Remove this?  And header.
void rdb_rget_secondary_slice(
        rockshard rocksh,
        uuid_u sindex_uuid,
        btree_slice_t *slice,
        const region_t &shard,
        const ql::datumspec_t &datumspec,
        const key_range_t &sindex_region_range,
        real_superblock_lock *superblock,
        ql::env_t *ql_env,
        const ql::batchspec_t &batchspec,
        const std::vector<transform_variant_t> &transforms,
        const optional<terminal_variant_t> &terminal,
        const key_range_t &pk_range,
        sorting_t sorting,
        require_sindexes_t require_sindex_val,
        const sindex_disk_info_t &sindex_info,
        rget_read_response_t *response,
        release_superblock_t release_superblock) {
    r_sanity_check(boost::get<ql::exc_t>(&response->result) == nullptr);
    guarantee(sindex_info.geo == sindex_geo_bool_t::REGULAR);
    PROFILE_STARTER_IF_ENABLED(
        ql_env->profile() == profile_bool_t::PROFILE,
        "Do range scan on secondary index.",
        ql_env->trace);

    const reql_version_t sindex_func_reql_version =
        sindex_info.mapping_version_info.latest_compatible_reql_version;

    key_range_t active_region_range = sindex_region_range;
    rocks_rget_secondary_cb callback(
        rget_io_data_t(response, slice),
        job_data_t(ql_env,
                   batchspec,
                   transforms,
                   terminal,
                   shard,
                   !reversed(sorting)
                       ? sindex_region_range.left
                       : sindex_region_range.right.key_or_max(),
                   sorting,
                   require_sindex_val),
        rget_sindex_data_t(
            pk_range,
            datumspec,
            &active_region_range,
            sindex_func_reql_version,
            sindex_info.mapping,
            sindex_info.multi));

    std::string rocks_kv_prefix = rockstore::table_secondary_prefix(rocksh.table_id, rocksh.shard_no, sindex_uuid);

    // TODO: We could make a rocksdb snapshot here, and iterate through that,
    // instead of holding a superblock.
    // TODO: Or we could wait for the superblock here, or wait for it outside and don't even pass the superblock in.
    direction_t direction = reversed(sorting) ? direction_t::backward : direction_t::forward;
    auto cb = [&](const std::pair<ql::datum_range_t, uint64_t> &pair, bool is_last) {
        key_range_t sindex_keyrange =
            pair.first.to_sindex_keyrange(sindex_func_reql_version);
        rocks_rget_secondary_cb_wrapper wrapper(
            &callback,
            pair.second,
            key_to_unescaped_str(sindex_keyrange.left));
        key_range_t active_range = active_region_range.intersection(sindex_keyrange);
        // This can happen sometimes with truncated keys.
        if (active_range.is_empty()) {
            return continue_bool_t::CONTINUE;
        }
        superblock->read_acq_signal()->wait_lazily_ordered();
        rockstore::snapshot snap = make_snapshot(rocksh.rocks);
        if (is_last && release_superblock == release_superblock_t::RELEASE) {
            superblock->reset_superblock();
        }
        return rocks_traversal(
            rocksh.rocks,
            snap.snap,
            rocks_kv_prefix,
            active_range,
            direction,
            &wrapper);
    };
    continue_bool_t cont = datumspec.iter(sorting, cb);
    callback.finish(cont);
}

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
        rget_read_response_t *response) {
    guarantee(query_geometry.has());

    guarantee(sindex_info.geo == sindex_geo_bool_t::GEO);
    PROFILE_STARTER_IF_ENABLED(
        ql_env->profile() == profile_bool_t::PROFILE,
        "Do intersection scan on geospatial index.",
        ql_env->trace);

    const reql_version_t sindex_func_reql_version =
        sindex_info.mapping_version_info.latest_compatible_reql_version;
    collect_all_geo_intersecting_cb_t callback(
        slice,
        geo_job_data_t(ql_env,
                       shard,
                       // The sorting is never `DESCENDING`, so this is always right.
                       sindex_range.left,
                       batchspec,
                       transforms,
                       terminal,
                       is_stamp_read),
        geo_sindex_data_t(pk_range, sindex_info.mapping,
                          sindex_func_reql_version, sindex_info.multi),
        query_geometry,
        response);

    continue_bool_t cont = geo_traversal(
        snap, rocksh, sindex_uuid, sindex_range, &callback);
    callback.finish(cont);
}


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
    nearest_geo_read_response_t *response) {

    guarantee(sindex_info.geo == sindex_geo_bool_t::GEO);
    PROFILE_STARTER_IF_ENABLED(
        ql_env->profile() == profile_bool_t::PROFILE,
        "Do nearest traversal on geospatial index.",
        ql_env->trace);

    const reql_version_t sindex_func_reql_version =
        sindex_info.mapping_version_info.latest_compatible_reql_version;

    // TODO (daniel): Instead of calling this multiple times until we are done,
    //   results should be streamed lazily. Also, even if we don't do that,
    //   the copying of the result we do here is bad.
    nearest_traversal_state_t state(center, max_results, max_dist, geo_system);
    response->results_or_error = nearest_geo_read_response_t::result_t();
    do {
        nearest_geo_read_response_t partial_response;
        try {
            nearest_traversal_cb_t callback(
                slice,
                geo_sindex_data_t(pk_range, sindex_info.mapping,
                                  sindex_func_reql_version, sindex_info.multi),
                ql_env,
                &state);
            geo_traversal(
                snap, rocksh, sindex_uuid, key_range_t::universe(), &callback);
            callback.finish(&partial_response);
        } catch (const geo_exception_t &e) {
            partial_response.results_or_error =
                ql::exc_t(ql::base_exc_t::LOGIC, e.what(),
                          ql::backtrace_id_t::empty());
        }
        if (boost::get<ql::exc_t>(&partial_response.results_or_error)) {
            response->results_or_error = partial_response.results_or_error;
            return;
        } else {
            auto partial_res = boost::get<nearest_geo_read_response_t::result_t>(
                &partial_response.results_or_error);
            guarantee(partial_res != nullptr);
            auto full_res = boost::get<nearest_geo_read_response_t::result_t>(
                &response->results_or_error);
            std::move(partial_res->begin(), partial_res->end(),
                      std::back_inserter(*full_res));
        }
    } while (state.proceed_to_next_batch() == continue_bool_t::CONTINUE);
}

// TODO: Make sure that distribution gets (with new rockstore stuff) get tested.
void rdb_distribution_get(rockshard rocksh, int keys_limit,
                          const key_range_t &key_range,
                          distribution_read_response_t *response) {
    std::vector<store_key_t> key_lowerbounds;
    std::vector<uint64_t> interval_disk_sizes;
    get_distribution(rocksh, key_range, keys_limit,
                    &key_lowerbounds, &interval_disk_sizes);
    // TODO: Return a vec of pairs in get_distribution.
    guarantee(key_lowerbounds.size() == interval_disk_sizes.size());

    // TODO: We're changing from key counts to disk size.  This means a rocksdb
    // shard is not compatible with the cluster.

    for (size_t i = 0; i < key_lowerbounds.size(); ++i) {
        response->key_counts[key_lowerbounds[i]] = interval_disk_sizes[i];
    }
}

static const int8_t HAS_VALUE = 0;
static const int8_t HAS_NO_VALUE = 1;

template <cluster_version_t W>
void serialize(write_message_t *wm, const rdb_modification_info_t &info) {
    if (!info.deleted.first.has()) {
        serialize<W>(wm, HAS_NO_VALUE);
    } else {
        serialize<W>(wm, HAS_VALUE);
        serialize<W>(wm, info.deleted.first);
    }

    if (!info.added.first.has()) {
        serialize<W>(wm, HAS_NO_VALUE);
    } else {
        serialize<W>(wm, HAS_VALUE);
        serialize<W>(wm, info.added.first);
    }
}

template <cluster_version_t W>
archive_result_t deserialize(read_stream_t *s, rdb_modification_info_t *info) {
    int8_t has_value;
    archive_result_t res = deserialize<W>(s, &has_value);
    if (bad(res)) { return res; }

    if (has_value == HAS_VALUE) {
        res = deserialize<W>(s, &info->deleted.first);
        if (bad(res)) { return res; }
    }

    res = deserialize<W>(s, &has_value);
    if (bad(res)) { return res; }

    if (has_value == HAS_VALUE) {
        res = deserialize<W>(s, &info->added.first);
        if (bad(res)) { return res; }
    }

    return archive_result_t::SUCCESS;
}

INSTANTIATE_SERIALIZABLE_SINCE_v1_13(rdb_modification_info_t);

RDB_IMPL_SERIALIZABLE_2_SINCE_v1_13(rdb_modification_report_t, primary_key, info);

rdb_modification_report_cb_t::rdb_modification_report_cb_t(
        store_t *store,
        real_superblock_lock *sindex_block,
        auto_drainer_t::lock_t lock)
    : lock_(lock), store_(store),
      sindex_block_(sindex_block) {
    store_->acquire_all_sindex_superblocks_for_write(sindex_block_, &sindexes_);
}

rdb_modification_report_cb_t::~rdb_modification_report_cb_t() { }

bool rdb_modification_report_cb_t::has_pkey_cfeeds(
    const std::vector<store_key_t> &keys) {
    const store_key_t *min = nullptr, *max = nullptr;
    for (const auto &key : keys) {
        if (min == nullptr || key < *min) min = &key;
        if (max == nullptr || key > *max) max = &key;
    }
    if (min != nullptr && max != nullptr) {
        key_range_t range(key_range_t::closed, *min,
                          key_range_t::closed, *max);
        auto cservers = store_->access_changefeed_servers();
        for (auto &&pair : *cservers.first) {
            if (pair.first.overlaps(range)
                && pair.second->has_limit(r_nullopt,
                                          pair.second->get_keepalive())) {
                return true;
            }
        }
    }
    return false;
}

void rdb_modification_report_cb_t::finish(
    btree_slice_t *btree, real_superblock_lock *superblock) {
    auto cservers = store_->access_changefeed_servers();
    for (auto &&pair : *cservers.first) {
        pair.second->foreach_limit(
            r_nullopt,
            r_nullopt,
            nullptr,
            [&](rwlock_in_line_t *clients_spot,
                rwlock_in_line_t *limit_clients_spot,
                rwlock_in_line_t *lm_spot,
                ql::changefeed::limit_manager_t *lm) {
                if (!lm->drainer.is_draining()) {
                    auto lock = lm->drainer.lock();
                    guarantee(clients_spot->read_signal()->is_pulsed());
                    guarantee(limit_clients_spot->read_signal()->is_pulsed());
                    lm->commit(lm_spot,
                               ql::changefeed::primary_ref_t{btree, superblock});
                }
            }, pair.second->get_keepalive());
    }
}

new_mutex_in_line_t rdb_modification_report_cb_t::get_in_line_for_sindex() {
    return store_->get_in_line_for_sindex_queue(sindex_block_);
}
rwlock_in_line_t rdb_modification_report_cb_t::get_in_line_for_cfeed_stamp() {
    return store_->get_in_line_for_cfeed_stamp(access_t::write);
}

void rdb_modification_report_cb_t::on_mod_report(
    rockshard rocksh,
    const rdb_modification_report_t &report,
    bool update_pkey_cfeeds,
    new_mutex_in_line_t *sindex_spot,
    rwlock_in_line_t *cfeed_stamp_spot) {
    if (report.info.deleted.first.has() || report.info.added.first.has()) {
        // We spawn the sindex update in its own coroutine because we don't want to
        // hold the sindex update for the changefeed update or vice-versa.
        cond_t sindexes_updated_cond, keys_available_cond;
        index_vals_t old_cfeed_keys, new_cfeed_keys;
        sindex_spot->acq_signal()->wait_lazily_unordered();
        coro_t::spawn_now_dangerously(
            std::bind(&rdb_modification_report_cb_t::on_mod_report_sub,
                      this,
                      rocksh,
                      report,
                      sindex_spot,
                      &keys_available_cond,
                      &sindexes_updated_cond,
                      &old_cfeed_keys,
                      &new_cfeed_keys));
        auto cserver = store_->changefeed_server(report.primary_key);
        if (update_pkey_cfeeds && cserver.first != nullptr) {
            cserver.first->foreach_limit(
                r_nullopt,
                r_nullopt,
                &report.primary_key,
                [&](rwlock_in_line_t *clients_spot,
                    rwlock_in_line_t *limit_clients_spot,
                    rwlock_in_line_t *lm_spot,
                    ql::changefeed::limit_manager_t *lm) {
                    if (!lm->drainer.is_draining()) {
                        auto lock = lm->drainer.lock();
                        guarantee(clients_spot->read_signal()->is_pulsed());
                        guarantee(limit_clients_spot->read_signal()->is_pulsed());
                        if (report.info.deleted.first.has()) {
                            lm->del(lm_spot, report.primary_key, is_primary_t::YES);
                        }
                        if (report.info.added.first.has()) {
                            // The conflicting null values are resolved by
                            // the primary key.
                            lm->add(lm_spot, report.primary_key, is_primary_t::YES,
                                    ql::datum_t::null(), report.info.added.first);
                        }
                    }
                }, cserver.second);
        }
        keys_available_cond.wait_lazily_unordered();
        if (cserver.first != nullptr) {
            cserver.first->send_all(
                ql::changefeed::msg_t(
                    ql::changefeed::msg_t::change_t{
                        old_cfeed_keys,
                        new_cfeed_keys,
                        report.primary_key,
                        report.info.deleted.first,
                        report.info.added.first}),
                report.primary_key,
                cfeed_stamp_spot,
                cserver.second);
        }
        sindexes_updated_cond.wait_lazily_unordered();
    }
}

void rdb_modification_report_cb_t::on_mod_report_sub(
    rockshard rocksh,
    const rdb_modification_report_t &mod_report,
    new_mutex_in_line_t *spot,
    cond_t *keys_available_cond,
    cond_t *done_cond,
    index_vals_t *cfeed_old_keys_out,
    index_vals_t *cfeed_new_keys_out) {
    store_->sindex_queue_push(mod_report, spot);
    rdb_update_sindexes(rocksh,
                        store_,
                        sindex_block_,
                        sindexes_,
                        &mod_report,
                        keys_available_cond,
                        cfeed_old_keys_out,
                        cfeed_new_keys_out);
    guarantee(keys_available_cond->is_pulsed());
    done_cond->pulse();
}

std::vector<std::string> expand_geo_key(
        const ql::datum_t &key,
        const store_key_t &primary_key,
        optional<uint64_t> tag_num) {
    // Ignore non-geometry objects in geo indexes.
    // TODO (daniel): This needs to be changed once compound geo index
    // support gets added.
    if (!key.is_ptype(ql::pseudo::geometry_string)) {
        return std::vector<std::string>();
    }

    try {
        std::vector<std::string> grid_keys =
            compute_index_grid_keys(key, GEO_INDEX_GOAL_GRID_CELLS);

        std::vector<std::string> result;
        result.reserve(grid_keys.size());
        for (size_t i = 0; i < grid_keys.size(); ++i) {
            // TODO (daniel): Something else that needs change for compound index
            //   support: We must be able to truncate geo keys and handle such
            //   truncated keys.
            rassert(grid_keys[i].length() <= ql::datum_t::trunc_size(
                        key_to_unescaped_str(primary_key).length()));

            result.push_back(
                ql::datum_t::compose_secondary(
                    grid_keys[i], primary_key, tag_num));
        }

        return result;
    } catch (const geo_exception_t &e) {
        // As things are now, this exception is actually ignored in
        // `compute_keys()`. That's ok, though it would be nice if we could
        // pass on some kind of warning to the user.
        logWRN("Failed to compute grid keys for an index: %s", e.what());
        rfail_target(&key, ql::base_exc_t::LOGIC,
                "Failed to compute grid keys: %s", e.what());
    }
}

void compute_keys(const store_key_t &primary_key,
                  ql::datum_t doc,
                  const sindex_disk_info_t &index_info,
                  std::vector<std::pair<store_key_t, ql::datum_t> > *keys_out,
                  std::vector<index_pair_t> *cfeed_keys_out) {

    guarantee(keys_out->empty());

    const reql_version_t reql_version =
        index_info.mapping_version_info.latest_compatible_reql_version;

    // Secondary index functions are deterministic (so no need for an rdb_context_t)
    // and evaluated in a pristine environment (without global optargs).
    cond_t non_interruptor;
    ql::env_t sindex_env(&non_interruptor,
                         ql::return_empty_normal_batches_t::NO,
                         reql_version);

    ql::datum_t index =
        index_info.mapping.compile_wire_func()->call(&sindex_env, doc)->as_datum();

    if (index_info.multi == sindex_multi_bool_t::MULTI
        && index.get_type() == ql::datum_t::R_ARRAY) {
        for (uint64_t i = 0; i < index.arr_size(); ++i) {
            const ql::datum_t &skey = index.get(i, ql::THROW);
            if (index_info.geo == sindex_geo_bool_t::GEO) {
                std::vector<std::string> geo_keys = expand_geo_key(skey,
                                                                   primary_key,
                                                                   make_optional(i));
                for (auto it = geo_keys.begin(); it != geo_keys.end(); ++it) {
                    keys_out->push_back(std::make_pair(store_key_t(*it), skey));
                }
                if (cfeed_keys_out != nullptr) {
                    // For geospatial indexes, we generate multiple keys for the same
                    // index entry. We only pass the smallest one on in order to not get
                    // redundant results on the changefeed.
                    auto min_it = std::min_element(geo_keys.begin(), geo_keys.end());
                    if (min_it != geo_keys.end()) {
                        cfeed_keys_out->push_back(
                            std::make_pair(skey, std::move(*min_it)));
                    }
                }
            } else {
                try {
                    std::string store_key =
                        skey.print_secondary(reql_version, primary_key,
                                             make_optional(i));
                    keys_out->push_back(
                        std::make_pair(store_key_t(store_key), skey));
                    if (cfeed_keys_out != nullptr) {
                        cfeed_keys_out->push_back(
                            std::make_pair(skey, std::move(store_key)));
                    }
                } catch (const ql::base_exc_t &e) {
                    if (reql_version < reql_version_t::v2_1) {
                        throw;
                    }
                    // One of the values couldn't be converted to an index key.
                    // Ignore it and move on to the next one.
                }
            }
        }
    } else {
        if (index_info.geo == sindex_geo_bool_t::GEO) {
            std::vector<std::string> geo_keys = expand_geo_key(index,
                                                               primary_key,
                                                               r_nullopt);
            for (auto it = geo_keys.begin(); it != geo_keys.end(); ++it) {
                keys_out->push_back(std::make_pair(store_key_t(*it), index));
            }
            if (cfeed_keys_out != nullptr) {
                // For geospatial indexes, we generate multiple keys for the same
                // index entry. We only pass the smallest one on in order to not get
                // redundant results on the changefeed.
                auto min_it = std::min_element(geo_keys.begin(), geo_keys.end());
                if (min_it != geo_keys.end()) {
                    cfeed_keys_out->push_back(
                        std::make_pair(index, std::move(*min_it)));
                }
            }
        } else {
            std::string store_key =
                index.print_secondary(reql_version, primary_key, r_nullopt);
            keys_out->push_back(
                std::make_pair(store_key_t(store_key), index));
            if (cfeed_keys_out != nullptr) {
                cfeed_keys_out->push_back(
                    std::make_pair(index, std::move(store_key)));
            }
        }
    }
}

void serialize_sindex_info(write_message_t *wm,
                           const sindex_disk_info_t &info) {
    serialize_cluster_version(wm, cluster_version_t::LATEST_DISK);
    serialize<cluster_version_t::LATEST_DISK>(
        wm, info.mapping_version_info.original_reql_version);
    serialize<cluster_version_t::LATEST_DISK>(
        wm, info.mapping_version_info.latest_compatible_reql_version);
    serialize<cluster_version_t::LATEST_DISK>(
        wm, info.mapping_version_info.latest_checked_reql_version);

    serialize<cluster_version_t::LATEST_DISK>(wm, info.mapping);
    serialize<cluster_version_t::LATEST_DISK>(wm, info.multi);
    serialize<cluster_version_t::LATEST_DISK>(wm, info.geo);
}

void deserialize_sindex_info(
        const std::vector<char> &data,
        sindex_disk_info_t *info_out,
        const std::function<void(obsolete_reql_version_t)> &obsolete_cb) {
    buffer_read_stream_t read_stream(data.data(), data.size());
    // This cluster version field is _not_ a ReQL evaluation version field, which is
    // in secondary_index_t -- it only says how the value was serialized.
    cluster_version_t cluster_version;
    static_assert(obsolete_cluster_version_t::v1_13_2_is_latest
                  == obsolete_cluster_version_t::v1_13_2,
                  "1.13 is no longer the only obsolete cluster version.  "
                  "Instead of passing a constant obsolete_reql_version_t::v1_13 into "
                  "`obsolete_cb` below, there should be a separate `obsolete_cb` to "
                  "handle the different obsolete cluster versions.");
    archive_result_t success;

    try {
        success = deserialize_cluster_version(
            &read_stream,
            &cluster_version,
            std::bind(obsolete_cb, obsolete_reql_version_t::v1_13));

    } catch (const archive_exc_t &e) {
        rfail_toplevel(ql::base_exc_t::INTERNAL,
                       "Unrecognized secondary index version,"
                       " secondary index not created.");
    }
    throw_if_bad_deserialization(success, "sindex description");

    switch (cluster_version) {
    case cluster_version_t::v1_14:
    case cluster_version_t::v1_15:
    case cluster_version_t::v1_16:
    case cluster_version_t::v2_0:
    case cluster_version_t::v2_1:
    case cluster_version_t::v2_2:
    case cluster_version_t::v2_3:
    case cluster_version_t::v2_4:
    case cluster_version_t::v2_5_is_latest:
        success = deserialize_reql_version(
                &read_stream,
                &info_out->mapping_version_info.original_reql_version,
                obsolete_cb);
        throw_if_bad_deserialization(success, "original_reql_version");
        success = deserialize_for_version(
                cluster_version,
                &read_stream,
                &info_out->mapping_version_info.latest_compatible_reql_version);
        throw_if_bad_deserialization(success, "latest_compatible_reql_version");
        success = deserialize_for_version(
                cluster_version,
                &read_stream,
                &info_out->mapping_version_info.latest_checked_reql_version);
        throw_if_bad_deserialization(success, "latest_checked_reql_version");
        break;
    default:
        unreachable();
    }

    success = deserialize_for_version(cluster_version,
        &read_stream, &info_out->mapping);
    throw_if_bad_deserialization(success, "sindex description");

    success = deserialize_for_version(cluster_version, &read_stream, &info_out->multi);
    throw_if_bad_deserialization(success, "sindex description");
    switch (cluster_version) {
    case cluster_version_t::v1_14:
        info_out->geo = sindex_geo_bool_t::REGULAR;
        break;
    case cluster_version_t::v1_15: // fallthru
    case cluster_version_t::v1_16: // fallthru
    case cluster_version_t::v2_0: // fallthru
    case cluster_version_t::v2_1: // fallthru
    case cluster_version_t::v2_2: // fallthru
    case cluster_version_t::v2_3: // fallthru
    case cluster_version_t::v2_4: // fallthru
    case cluster_version_t::v2_5_is_latest:
        success = deserialize_for_version(cluster_version, &read_stream, &info_out->geo);
        throw_if_bad_deserialization(success, "sindex description");
        break;
    default: unreachable();
    }
    guarantee(static_cast<size_t>(read_stream.tell()) == data.size(),
              "An sindex description was incompletely deserialized.");
}

void deserialize_sindex_info_or_crash(
        const std::vector<char> &data,
        sindex_disk_info_t *info_out)
            THROWS_ONLY(archive_exc_t) {
    deserialize_sindex_info(data, info_out,
        [](obsolete_reql_version_t ver) -> void {
            switch (ver) {
            case obsolete_reql_version_t::v1_13:
                fail_due_to_user_error("Encountered a RethinkDB 1.13 secondary index, "
                                       "which is no longer supported.  You can use "
                                       "RethinkDB 2.0.5 to update your secondary index.");
                break;
            // v1_15 is equal to v1_14
            case obsolete_reql_version_t::v1_14:
                fail_due_to_user_error("Encountered an index from before RethinkDB "
                                       "1.16, which is no longer supported.  You can "
                                       "use RethinkDB 2.1 to update your secondary "
                                       "index.");
                break;
            default: unreachable();
            }
        });
}

/* Used below by rdb_update_sindexes. */
void rdb_update_single_sindex(
        rockshard rocksh,
        store_t *store,
        real_superblock_lock *superblock,
        const store_t::sindex_access_t *sindex,
        const rdb_modification_report_t *modification,
        size_t *updates_left,
        auto_drainer_t::lock_t,
        cond_t *keys_available_cond,
        std::vector<index_pair_t> *cfeed_old_keys_out,
        std::vector<index_pair_t> *cfeed_new_keys_out)
    THROWS_NOTHING {
    // Note if you get this error it's likely that you've passed in a default
    // constructed mod_report. Don't do that.  Mod reports should always be passed
    // to a function as an output parameter before they're passed to this
    // function.
    guarantee(modification->primary_key.size() != 0);

    guarantee(cfeed_old_keys_out == nullptr || cfeed_old_keys_out->size() == 0);
    guarantee(cfeed_new_keys_out == nullptr || cfeed_new_keys_out->size() == 0);

    sindex_disk_info_t sindex_info;
    try {
        deserialize_sindex_info_or_crash(sindex->sindex.opaque_definition, &sindex_info);
    } catch (const archive_exc_t &e) {
        crash("%s", e.what());
    }
    // TODO(2015-01): Actually get real profiling information for
    // secondary index updates.
    UNUSED profile::trace_t *const trace = nullptr;

    auto cserver = store->changefeed_server(modification->primary_key);

    if (modification->info.deleted.first.has()) {
        try {
            ql::datum_t deleted = modification->info.deleted.first;

            std::vector<std::pair<store_key_t, ql::datum_t> > keys;
            compute_keys(
                modification->primary_key, deleted, sindex_info,
                &keys, cfeed_old_keys_out);
            if (cserver.first != nullptr) {
                cserver.first->foreach_limit(
                    make_optional(sindex->name.name),
                    make_optional(sindex->sindex.id),
                    &modification->primary_key,
                    [&](rwlock_in_line_t *clients_spot,
                        rwlock_in_line_t *limit_clients_spot,
                        rwlock_in_line_t *lm_spot,
                        ql::changefeed::limit_manager_t *lm) {
                        guarantee(clients_spot->read_signal()->is_pulsed());
                        guarantee(limit_clients_spot->read_signal()->is_pulsed());
                        for (const auto &pair : keys) {
                            lm->del(lm_spot, pair.first, is_primary_t::NO);
                        }
                    }, cserver.second);
            }
            for (auto it = keys.begin(); it != keys.end(); ++it) {
                std::string rocks_secondary_kv_location
                    = rockstore::table_secondary_key(
                        rocksh.table_id,
                        rocksh.shard_no,
                        sindex->sindex.id,
                        key_to_unescaped_str(it->first));

                // TODO: We could do a SingleDelete for secondary index removals.

                superblock->wait_write_batch()->Delete(rocks_secondary_kv_location);
            }
        } catch (const ql::base_exc_t &) {
            // Do nothing (it wasn't actually in the index).

            // See comment in `catch` below.
            guarantee(cfeed_old_keys_out == nullptr || cfeed_old_keys_out->size() == 0);
        }
    }

    // If the secondary index is being deleted, we don't add any new values to
    // the sindex tree.
    // This is so we don't race against any sindex erase about who is faster
    // (we with inserting new entries, or the erase with removing them).
    const bool sindex_is_being_deleted = sindex->sindex.being_deleted;
    if (!sindex_is_being_deleted && modification->info.added.first.has()) {
        bool decremented_updates_left = false;
        try {
            ql::datum_t added = modification->info.added.first;

            std::vector<std::pair<store_key_t, ql::datum_t> > keys;

            compute_keys(
                modification->primary_key, added, sindex_info,
                &keys, cfeed_new_keys_out);
            if (keys_available_cond != nullptr) {
                guarantee(*updates_left > 0);
                decremented_updates_left = true;
                if (--*updates_left == 0) {
                    keys_available_cond->pulse();
                }
            }
            if (cserver.first != nullptr) {
                cserver.first->foreach_limit(
                    make_optional(sindex->name.name),
                    make_optional(sindex->sindex.id),
                    &modification->primary_key,
                    [&](rwlock_in_line_t *clients_spot,
                        rwlock_in_line_t *limit_clients_spot,
                        rwlock_in_line_t *lm_spot,
                        ql::changefeed::limit_manager_t *lm) {
                        guarantee(clients_spot->read_signal()->is_pulsed());
                        guarantee(limit_clients_spot->read_signal()->is_pulsed());
                        for (const auto &pair :keys) {
                            lm->add(lm_spot, pair.first, is_primary_t::NO,
                                    pair.second, added);
                        }
                    }, cserver.second);
            }
            for (auto it = keys.begin(); it != keys.end(); ++it) {
                std::string rocks_secondary_kv_location
                    = rockstore::table_secondary_key(
                        rocksh.table_id,
                        rocksh.shard_no,
                        sindex->sindex.id,
                        key_to_unescaped_str(it->first));

                // NOTE: We generally need a copy of the value to be available in the secondary index
                // because the secondary index key can get truncated.  TODO: Make it not get truncated,
                // so that we can have a reference to the primary key instead.
                ql::serialization_result_t res =
                    kv_location_set_secondary(
                        rocksh.rocks,
                        superblock,
                        rocks_secondary_kv_location,
                        modification->info.added.first /* copy of the value here */);
                // this particular context cannot fail AT THE MOMENT.
                guarantee(!bad(res));
            }
        } catch (const ql::base_exc_t &) {
            // Do nothing (we just drop the row from the index).

            // If we've decremented `updates_left` already, that means we might
            // have sent a change with new values for this key even though we're
            // actually dropping the row.  I *believe* that this catch statement
            // can only be triggered by an exception thrown from `compute_keys`
            // (which begs the question of why so many other statements are
            // inside of it), so this guarantee should never trip.
            if (keys_available_cond != nullptr) {
                guarantee(!decremented_updates_left);
                guarantee(cfeed_new_keys_out->size() == 0);
                guarantee(*updates_left > 0);
                if (--*updates_left == 0) {
                    keys_available_cond->pulse();
                }
            }
        }
    } else {
        if (keys_available_cond != nullptr) {
            guarantee(*updates_left > 0);
            if (--*updates_left == 0) {
                keys_available_cond->pulse();
            }
        }
    }

    if (cserver.first != nullptr) {
        cserver.first->foreach_limit(
            make_optional(sindex->name.name),
            make_optional(sindex->sindex.id),
            &modification->primary_key,
            [&](rwlock_in_line_t *clients_spot,
                rwlock_in_line_t *limit_clients_spot,
                rwlock_in_line_t *lm_spot,
                ql::changefeed::limit_manager_t *lm) {
                guarantee(clients_spot->read_signal()->is_pulsed());
                guarantee(limit_clients_spot->read_signal()->is_pulsed());
                lm->commit(lm_spot, ql::changefeed::sindex_ref_t{
                        sindex->btree, superblock, &sindex_info, sindex->sindex.id});
            }, cserver.second);
    }
}

void rdb_update_sindexes(
    rockshard rocksh,
    store_t *store,
    real_superblock_lock *superblock,
    const store_t::sindex_access_vector_t &sindexes,
    const rdb_modification_report_t *modification,
    cond_t *keys_available_cond,
    index_vals_t *cfeed_old_keys_out,
    index_vals_t *cfeed_new_keys_out) {

    {
        auto_drainer_t drainer;

        // This assert is here to make sure that we pulse keys_available_cond even if
        // some indexes aren't done constructing yet.
        ASSERT_NO_CORO_WAITING;

        size_t counter = 0;
        for (const auto &sindex : sindexes) {
            // Update only indexes that have been post-constructed for the relevant
            // range.
            if (!sindex->sindex.needs_post_construction_range.contains_key(
                    modification->primary_key)) {
                ++counter;
                superblock->write_acq_signal()->wait();
                coro_t::spawn_sometime(
                    std::bind(
                        &rdb_update_single_sindex,
                        rocksh,
                        store,
                        superblock,
                        sindex.get(),
                        modification,
                        &counter,
                        auto_drainer_t::lock_t(&drainer),
                        keys_available_cond,
                        cfeed_old_keys_out == nullptr
                            ? nullptr
                            : &(*cfeed_old_keys_out)[sindex->name.name],
                        cfeed_new_keys_out == nullptr
                            ? nullptr
                            : &(*cfeed_new_keys_out)[sindex->name.name]));
            }
        }
        if (counter == 0 && keys_available_cond != nullptr) {
            keys_available_cond->pulse();
        }
    }
}

class post_construct_traversal_helper_t : public rocks_traversal_cb {
public:
    post_construct_traversal_helper_t(
            store_t *store,
            const std::set<uuid_u> &sindexes_to_post_construct,
            cond_t *on_indexes_deleted,
            const std::function<bool(int64_t)> &check_should_abort,
            signal_t *interruptor)
        : store_(store),
          sindexes_to_post_construct_(sindexes_to_post_construct),
          on_indexes_deleted_(on_indexes_deleted),
          interruptor_(interruptor),
          check_should_abort_(check_should_abort),
          pairs_constructed_(0),
          stopped_before_completion_(false),
          current_chunk_size_(0) {
        // Start an initial write transaction for the first chunk.
        // (this acquisition should never block)
        new_mutex_acq_t wtxn_acq(&wtxn_lock_);
        start_write_transaction(&wtxn_acq);
    }

    ~post_construct_traversal_helper_t() {
        sindexes_.clear();
        if (wtxn_.has()) {
            wtxn_->commit(store_->rocksh().rocks, std::move(superblock_));
        }
    }

    continue_bool_t handle_pair(
            std::pair<const char *, size_t> key, std::pair<const char *, size_t> value)
            THROWS_ONLY(interrupted_exc_t) override {

        if (interruptor_->is_pulsed() || on_indexes_deleted_->is_pulsed()) {
            throw interrupted_exc_t();
        }

        // Account for the read value in the stats
        store_->btree->stats.pm_keys_read.record();
        store_->btree->stats.pm_total_keys_read += 1;

        // Grab the key and value and construct a modification report for the key/value
        // pair.
        const store_key_t primary_key(key.second, reinterpret_cast<const uint8_t *>(key.first));
        rdb_modification_report_t mod_report(primary_key);
        mod_report.info.added.first
            = datum_deserialize_from_vec(value.first, value.second);

        // Store the value into the secondary indexes
        {
            // We need this mutex because we don't want `wtxn` to be destructed,
            // but also because only one coroutine can be traversing the indexes
            // through `rdb_update_sindexes` at a time (or else the btree will get
            // corrupted!).
            new_mutex_acq_t wtxn_acq(&wtxn_lock_, interruptor_);
            guarantee(wtxn_.has());
            rdb_update_sindexes(store_->rocksh(),
                                store_,
                                superblock_.get(),
                                sindexes_,
                                &mod_report,
                                nullptr,
                                nullptr,
                                nullptr);
        }

        // Account for the sindex writes in the stats
        store_->btree->stats.pm_keys_set.record(sindexes_.size());
        store_->btree->stats.pm_total_keys_set += sindexes_.size();

        // Update the traversed range boundary (everything below here will happen in
        // key order).
        // This can't be interrupted, because we have already called rdb_update_sindexes,
        // so now we /must/ update traversed_right_bound.

        // TODO: Concurrent_traversal used to call waiter.wait() here (area
        // below being in-order mutexed), but now rocks traversal is in-order.
        // waiter.wait();
        traversed_right_bound_ = primary_key;

        // Release the write transaction and secondary index locks once we've reached the
        // designated chunk size. Then acquire a new transaction once the previous one
        // has been flushed.
        {
            new_mutex_acq_t wtxn_acq(&wtxn_lock_, interruptor_);
            ++current_chunk_size_;
            if (current_chunk_size_ >= MAX_CHUNK_SIZE) {
                current_chunk_size_ = 0;
                sindexes_.clear();
                wtxn_->commit(store_->rocksh().rocks, std::move(superblock_));
                wtxn_.reset();
                start_write_transaction(&wtxn_acq);
            }
        }

        ++pairs_constructed_;
        if (check_should_abort_(pairs_constructed_)) {
            stopped_before_completion_ = true;
            return continue_bool_t::ABORT;
        } else {
            return continue_bool_t::CONTINUE;
        }
    }

    store_key_t get_traversed_right_bound() const {
        return traversed_right_bound_;
    }

    bool stopped_before_completion() const {
        return stopped_before_completion_;
    }

private:
    // Number of key/value pairs we process before releasing the write transaction
    // and waiting for the secondary index data to be flushed to disk.
    // Also see the comment above `scoped_ptr_t<txn_t> wtxn;` below.
    static const int MAX_CHUNK_SIZE = 32;

    void start_write_transaction(new_mutex_acq_t *wtxn_acq) {
        wtxn_acq->guarantee_is_holding(&wtxn_lock_);
        guarantee(!wtxn_.has());

        // Start a write transaction and acquire the secondary indexes
        write_token_t token;
        store_->new_write_token(&token);

        // We use HARD durability because we want post construction
        // to be throttled if we insert data faster than it can
        // be written to disk. Otherwise we might exhaust the cache's
        // dirty page limit and bring down the whole table.
        // Other than that, the hard durability guarantee is not actually
        // needed here.
        store_->acquire_superblock_for_write(
                2 + MAX_CHUNK_SIZE,
                write_durability_t::HARD,
                &token,
                &wtxn_,
                &superblock_,
                interruptor_);

        // Acquire the sindex block and release the superblock.  (We don't
        // release the superblock anymore.)
        superblock_->sindex_block_write_signal()->wait_lazily_ordered();
        store_t::sindex_access_vector_t all_sindexes;
        store_->acquire_sindex_superblocks_for_write(
            make_optional(sindexes_to_post_construct_),
            superblock_.get(),
            &all_sindexes);

        // Filter out indexes that are being deleted. No need to keep post-constructing
        // those.
        guarantee(sindexes_.empty());
        for (auto &&access : all_sindexes) {
            if (!access->sindex.being_deleted) {
                sindexes_.emplace_back(std::move(access));
            }
        }
        if (sindexes_.empty()) {
            // All indexes have been deleted. Interrupt the traversal.
            on_indexes_deleted_->pulse_if_not_already_pulsed();
        }

        // We pretend that the indexes have been fully constructed, so that when we call
        // `rdb_update_sindexes` above, it actually updates the range we're currently
        // constructing. This is a bit hacky, but works.
        for (auto &&access : sindexes_) {
            access->sindex.needs_post_construction_range = key_range_t::empty();
        }
    }

    store_t *store_;
    const std::set<uuid_u> sindexes_to_post_construct_;
    cond_t *on_indexes_deleted_;
    signal_t *interruptor_;

    std::function<bool(int64_t)> check_should_abort_;

    // How far we've come in the traversal
    int64_t pairs_constructed_;
    store_key_t traversed_right_bound_;
    bool stopped_before_completion_;

    // We re-use a single write transaction and secondary index acquisition for a chunk
    // of writes to get better efficiency when flushing the index writes to disk.
    // We reset the transaction  after each chunk because large write transactions can
    // cause the cache to go into throttling, and that would interfere with other
    // transactions on this table.
    // Another aspect to keep in mind is that if we hold the write lock on the sindexes
    // for too long, other concurrent writes to parts of the secondary index that
    // are already live will also be delayed.
    scoped_ptr_t<txn_t> wtxn_;
    scoped_ptr_t<real_superblock_lock> superblock_;
    store_t::sindex_access_vector_t sindexes_;
    int current_chunk_size_;
    // Controls access to `sindexes_` and `wtxn_`.
    new_mutex_t wtxn_lock_;
};

void post_construct_secondary_index_range(
        store_t *store,
        const std::set<uuid_u> &sindex_ids_to_post_construct,
        key_range_t *construction_range_inout,
        const std::function<bool(int64_t)> &check_should_abort,
        signal_t *interruptor)
    THROWS_ONLY(interrupted_exc_t) {
    // TODO: Do read operations with rockstore.

    // In case the index gets deleted in the middle of the construction, this gets
    // triggered.
    cond_t on_index_deleted_interruptor;

    // Mind the destructor ordering.
    // The superblock must be released before txn (`btree_concurrent_traversal`
    // usually already takes care of that).
    // The txn must be destructed before the cache_account.
    cache_account_t cache_account;
    scoped_ptr_t<txn_t> txn;
    scoped_ptr_t<real_superblock_lock> superblock;

    // Start a snapshotted read transaction to traverse the primary btree
    read_token_t read_token;
    store->new_read_token(&read_token);
    store->acquire_superblock_for_read(
        &read_token,
        &txn,
        &superblock,
        interruptor);
    superblock->read_acq_signal()->wait_lazily_ordered();
    rockshard rocksh = store->rocksh();
    rockstore::snapshot rocksnap = make_snapshot(rocksh.rocks);
    superblock->reset_superblock();

    // Note: This starts a write transaction, which might get throttled.
    // It is important that we construct the `traversal_cb` *after* we've started
    // the snapshotted read transaction, or otherwise we might deadlock in the presence
    // of additional (unrelated) write transactions..
    post_construct_traversal_helper_t traversal_cb(
        store,
        sindex_ids_to_post_construct,
        &on_index_deleted_interruptor,
        check_should_abort,
        interruptor);

    cache_account
        = txn->cache()->create_cache_account(SINDEX_POST_CONSTRUCTION_CACHE_PRIORITY);
    txn->set_account(&cache_account);

    std::string rocks_kv_prefix = rockstore::table_primary_prefix(rocksh.table_id, rocksh.shard_no);
    continue_bool_t cont = rocks_traversal(
        rocksh.rocks,
        rocksnap.snap,
        rocks_kv_prefix,
        *construction_range_inout,
        direction_t::forward,
        &traversal_cb);
    rocksnap.reset();
    if (cont == continue_bool_t::ABORT
        && (interruptor->is_pulsed() || on_index_deleted_interruptor.is_pulsed())) {
        throw interrupted_exc_t();
    }

    // Update the left bound of the construction range
    if (!traversal_cb.stopped_before_completion()) {
        // The construction is done. Set the remaining range to empty.
        *construction_range_inout = key_range_t::empty();
    } else {
        // (the key_range_t construction below needs to be updated if the right bound can
        //  be bounded. For now we assume it's unbounded.)
        guarantee(construction_range_inout->right.unbounded);
        *construction_range_inout = key_range_t(
            key_range_t::bound_t::open, traversal_cb.get_traversed_right_bound(),
            key_range_t::bound_t::none, store_key_t());
    }
}
