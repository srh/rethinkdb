// Copyright 2010-2015 RethinkDB, all rights reserved.
#include <functional>

#include "arch/io/disk.hpp"
#include "arch/runtime/coroutines.hpp"
#include "arch/timing.hpp"
#include "btree/operations.hpp"
#include "btree/reql_specific.hpp"
#include "clustering/immediate_consistency/version.hpp"
#include "containers/archive/boost_types.hpp"
#include "containers/archive/vector_stream.hpp"
#include "containers/uuid.hpp"
#include "rapidjson/document.h"
#include "rdb_protocol/btree.hpp"
#include "rdb_protocol/datum_json.hpp"
#include "rdb_protocol/env.hpp"
#include "rdb_protocol/erase_range.hpp"
#include "rdb_protocol/minidriver.hpp"
#include "rdb_protocol/protocol.hpp"
#include "rdb_protocol/store.hpp"
#include "rdb_protocol/sym.hpp"
#include "stl_utils.hpp"
#include "unittest/gtest.hpp"
#include "unittest/unittest_utils.hpp"

#define TOTAL_KEYS_TO_INSERT 1000
#define MAX_RETRIES_FOR_SINDEX_POSTCONSTRUCT 50

namespace unittest {

void insert_rows(int start, int finish, store_t *store) {
    ql::configured_limits_t limits;

    guarantee(start <= finish);
    for (int i = start; i < finish; ++i) {
        cond_t dummy_interruptor;
        scoped_ptr_t<txn_t> txn;
        scoped_ptr_t<real_superblock_lock> superblock;
        {
            write_token_t token;
            store->new_write_token(&token);
            store->acquire_superblock_for_write(
                1, write_durability_t::SOFT,
                &token, &txn, &superblock, &dummy_interruptor);
            superblock->sindex_block_write_signal()->wait();

            std::string data = strprintf("{\"id\" : %d, \"sid\" : %d}", i, i * i);
            point_write_response_t response;

            store_key_t pk(ql::datum_t(static_cast<double>(i)).print_primary());
            rdb_modification_report_t mod_report(pk);
            rapidjson::Document doc;
            doc.Parse(data.c_str());
            promise_t<real_superblock_lock *> pass_back_superblock;
            // TODO: No passback anymore.
            rdb_set(
                store->rocksh(),
                pk,
                ql::to_datum(doc, limits, reql_version_t::LATEST),
                false, store->btree.get(), repli_timestamp_t::distant_past,
                superblock.get(), &response, &mod_report.info,
                static_cast<profile::trace_t *>(NULL), &pass_back_superblock);
            pass_back_superblock.wait();

            store_t::sindex_access_vector_t sindexes;
            store->acquire_all_sindex_superblocks_for_write(superblock.get(), &sindexes);
            rdb_update_sindexes(
                store->rocksh(),
                store,
                superblock.get(),
                sindexes,
                &mod_report,
                nullptr,
                nullptr,
                nullptr);

            new_mutex_in_line_t acq = store->get_in_line_for_sindex_queue(superblock.get());
            store->sindex_queue_push(mod_report, &acq);
        }
        txn->commit(store->rocksh().rocks, std::move(superblock));
    }
}

void insert_rows_and_pulse_when_done(int start, int finish,
        store_t *store, cond_t *pulse_when_done) {
    insert_rows(start, finish, store);
    pulse_when_done->pulse();
}

sindex_name_t create_sindex(store_t *store) {
    std::string name = uuid_to_str(generate_uuid());
    ql::sym_t one(1);
    ql::minidriver_t r(ql::backtrace_id_t::empty());
    ql::raw_term_t mapping = r.var(one)["sid"].root_term();
    sindex_config_t config(
        ql::map_wire_func_t(mapping, make_vector(one)),
        reql_version_t::LATEST,
        sindex_multi_bool_t::SINGLE,
        sindex_geo_bool_t::REGULAR);

    cond_t non_interruptor;
    store->sindex_create(name, config, &non_interruptor);

    return sindex_name_t(name);
}

void spawn_writes(store_t *store, cond_t *background_inserts_done) {
    coro_t::spawn_sometime(std::bind(&insert_rows_and_pulse_when_done,
                (TOTAL_KEYS_TO_INSERT * 9) / 10, TOTAL_KEYS_TO_INSERT,
                store, background_inserts_done));
}

ql::grouped_t<ql::stream_t> read_row_via_sindex(
        store_t *store,
        const sindex_name_t &sindex_name,
        int sindex_value) {
    cond_t dummy_interruptor;
    read_token_t token;
    store->new_read_token(&token);

    scoped_ptr_t<txn_t> txn;
    scoped_ptr_t<real_superblock_lock> superblock;

    store->acquire_superblock_for_read(
            &token, &txn, &superblock,
            &dummy_interruptor);

    uuid_u sindex_uuid;

    std::vector<char> opaque_definition;
    bool sindex_exists = store->acquire_sindex_superblock_for_read(
            sindex_name,
            "",
            superblock.get(),
            &opaque_definition,
            &sindex_uuid);
    guarantee(sindex_exists);

    sindex_disk_info_t sindex_info;
    try {
        deserialize_sindex_info_or_crash(opaque_definition, &sindex_info);
    } catch (const archive_exc_t &e) {
        crash("%s", e.what());
    }

    rget_read_response_t res;
    ql::datum_range_t datum_range(ql::datum_t(static_cast<double>(sindex_value)));
    /* The only thing this does is have a NULL `profile::trace_t *` in it which
     * prevents to profiling code from crashing. */
    ql::env_t dummy_env(&dummy_interruptor,
                        ql::return_empty_normal_batches_t::NO,
                        reql_version_t::LATEST);

    rdb_rget_secondary_slice(
        store->rocksh(),
        sindex_uuid,
        store->get_sindex_slice(sindex_uuid),
        region_t(),
        ql::datumspec_t(datum_range),
        datum_range.to_sindex_keyrange(reql_version_t::LATEST),
        superblock.get(),
        &dummy_env, // env_t
        ql::batchspec_t::default_for(ql::batch_type_t::NORMAL),
        std::vector<ql::transform_variant_t>(),
        optional<ql::terminal_variant_t>(),
        key_range_t::universe(),
        sorting_t::ASCENDING,
        require_sindexes_t::NO,
        sindex_info,
        &res,
        release_superblock_t::RELEASE);

    ql::grouped_t<ql::stream_t> *groups =
        boost::get<ql::grouped_t<ql::stream_t> >(&res.result);
    guarantee(groups != nullptr);
    return *groups;
}

void _check_keys_are_present(store_t *store,
        sindex_name_t sindex_name) {
    ql::configured_limits_t limits;
    for (int i = 0; i < TOTAL_KEYS_TO_INSERT; ++i) {
        ql::grouped_t<ql::stream_t> groups =
            read_row_via_sindex(store, sindex_name, i * i);
        ASSERT_EQ(1, groups.size());
        // The order of `groups` doesn't matter because this is a small unit test.
        ql::stream_t *stream = &groups.begin()->second;
        ASSERT_TRUE(stream != nullptr);
        ASSERT_EQ(1ul, stream->substreams.size());
        ql::raw_stream_t *raw_stream = &stream->substreams.begin()->second.stream;
        ASSERT_EQ(1ul, raw_stream->size());

        std::string expected_data = strprintf("{\"id\" : %d, \"sid\" : %d}", i, i * i);
        rapidjson::Document expected_value;
        expected_value.Parse(expected_data.c_str());
        ASSERT_EQ(ql::to_datum(expected_value, limits, reql_version_t::LATEST),
                  raw_stream->front().data);
    }
}

void check_keys_are_present(store_t *store,
        sindex_name_t sindex_name) {
    for (int i = 0; i < MAX_RETRIES_FOR_SINDEX_POSTCONSTRUCT; ++i) {
        try {
            _check_keys_are_present(store, sindex_name);
            return;
        } catch (const sindex_not_ready_exc_t&) { }
        /* Unfortunately we don't have an easy way right now to tell if the
         * sindex has actually been postconstructed so we just need to
         * check by polling. */
        nap(500);
    }
    ADD_FAILURE() << "Sindex still not available after many tries.";
}

void _check_keys_are_NOT_present(store_t *store,
        sindex_name_t sindex_name) {
    /* Check that we don't have any of the keys (we just deleted them all) */
    for (int i = 0; i < TOTAL_KEYS_TO_INSERT; ++i) {
        ql::grouped_t<ql::stream_t> groups =
            read_row_via_sindex(store, sindex_name, i * i);
        if (groups.size() != 0) {
            debugf_print("groups is non-empty", groups);
        }
        ASSERT_EQ(0, groups.size());
    }
}

void check_keys_are_NOT_present(store_t *store,
        sindex_name_t sindex_name) {
    for (int i = 0; i < MAX_RETRIES_FOR_SINDEX_POSTCONSTRUCT; ++i) {
        try {
            _check_keys_are_NOT_present(store, sindex_name);
            return;
        } catch (const sindex_not_ready_exc_t&) { }
        /* Unfortunately we don't have an easy way right now to tell if the
         * sindex has actually been postconstructed so we just need to
         * check by polling. */
        nap(500);
    }
    ADD_FAILURE() << "Sindex still not available after many tries.";
}

TPTEST(RDBBtree, SindexPostConstruct) {
    recreate_temporary_directory(base_path_t("."));
    temp_rockstore temp_rocks;
    io_backender_t io_backender(temp_rocks.rocks(), file_direct_io_mode_t::buffered_desired);

    store_t store(
            region_t::universe(),
            0,
            io_backender.rocks(),
            "unit_test_store",
            true,
            version_t::zero(),
            &get_global_perfmon_collection(),
            nullptr,
            &io_backender,
            base_path_t("."),
            generate_uuid(),
            update_sindexes_t::UPDATE);

    cond_t dummy_interruptor;

    insert_rows(0, (TOTAL_KEYS_TO_INSERT * 9) / 10, &store);

    sindex_name_t sindex_name = create_sindex(&store);

    cond_t background_inserts_done;
    spawn_writes(&store, &background_inserts_done);
    background_inserts_done.wait();

    check_keys_are_present(&store, sindex_name);
}

TPTEST(RDBBtree, SindexEraseRange) {
    recreate_temporary_directory(base_path_t("."));
    temp_rockstore temp_rocks;
    io_backender_t io_backender(temp_rocks.rocks(), file_direct_io_mode_t::buffered_desired);

    store_t store(
            region_t::universe(),
            0,
            io_backender.rocks(),
            "unit_test_store",
            true,
            version_t::zero(),
            &get_global_perfmon_collection(),
            nullptr,
            &io_backender,
            base_path_t("."),
            generate_uuid(),
            update_sindexes_t::UPDATE);

    cond_t dummy_interruptor;

    insert_rows(0, (TOTAL_KEYS_TO_INSERT * 9) / 10, &store);

    sindex_name_t sindex_name = create_sindex(&store);

    cond_t background_inserts_done;
    spawn_writes(&store, &background_inserts_done);
    background_inserts_done.wait();

    check_keys_are_present(&store, sindex_name);

    {
        /* Now we erase all of the keys we just inserted. */
        write_token_t token;
        store.new_write_token(&token);

        scoped_ptr_t<txn_t> txn;
        {
            scoped_ptr_t<real_superblock_lock> superblock;
            store.acquire_superblock_for_write(
                1,
                write_durability_t::SOFT,
                &token,
                &txn,
                &superblock,
                &dummy_interruptor);

            const key_range_t test_range = key_range_t::universe();
            rdb_protocol::range_key_tester_t tester(&test_range);
            superblock->sindex_block_write_signal()->wait();

            std::vector<rdb_modification_report_t> mod_reports;
            key_range_t deleted_range;
            rdb_erase_small_range(
                store.rocksh(),
                store.btree.get(),
                &tester,
                key_range_t::universe(),
                superblock.get(),
                &dummy_interruptor,
                0,
                &mod_reports,
                &deleted_range);

            store.update_sindexes(txn.get(), std::move(superblock), mod_reports);
        }
    }

    check_keys_are_NOT_present(&store, sindex_name);
}

TPTEST(RDBBtree, SindexInterruptionViaDrop) {
    recreate_temporary_directory(base_path_t("."));
    temp_rockstore temp_rocks;
    io_backender_t io_backender(temp_rocks.rocks(), file_direct_io_mode_t::buffered_desired);

    store_t store(
            region_t::universe(),
            0,
            io_backender.rocks(),
            "unit_test_store",
            true,
            version_t::zero(),
            &get_global_perfmon_collection(),
            nullptr,
            &io_backender,
            base_path_t("."),
            generate_uuid(),
            update_sindexes_t::UPDATE);

    cond_t dummy_interruptor;

    insert_rows(0, (TOTAL_KEYS_TO_INSERT * 9) / 10, &store);

    sindex_name_t sindex_name = create_sindex(&store);

    cond_t background_inserts_done;
    spawn_writes(&store, &background_inserts_done);

    store.sindex_drop(sindex_name.name, &dummy_interruptor);
    background_inserts_done.wait();
}

TPTEST(RDBBtree, SindexInterruptionViaStoreDelete) {
    recreate_temporary_directory(base_path_t("."));
    temp_rockstore temp_rocks;
    io_backender_t io_backender(temp_rocks.rocks(), file_direct_io_mode_t::buffered_desired);

    scoped_ptr_t<store_t> store(new store_t(
            region_t::universe(),
            0,
            io_backender.rocks(),
            "unit_test_store",
            true,
            version_t::zero(),
            &get_global_perfmon_collection(),
            nullptr,
            &io_backender,
            base_path_t("."),
            generate_uuid(),
            update_sindexes_t::UPDATE));

    insert_rows(0, (TOTAL_KEYS_TO_INSERT * 9) / 10, store.get());

    create_sindex(store.get());
    nap(1000);
    store.reset();
}

} //namespace unittest
