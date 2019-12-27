// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "unittest/gtest.hpp"

#include "arch/io/disk.hpp"
#include "btree/operations.hpp"
#include "btree/reql_specific.hpp"
#include "buffer_cache/alt.hpp"
#include "buffer_cache/page_cache.hpp"
#include "clustering/immediate_consistency/version.hpp"
#include "containers/uuid.hpp"
#include "unittest/unittest_utils.hpp"
#include "random.hpp"
#include "rdb_protocol/btree.hpp"
#include "rdb_protocol/datum.hpp"
#include "rdb_protocol/minidriver.hpp"
#include "rdb_protocol/store.hpp"
#include "rdb_protocol/protocol.hpp"

namespace unittest {

ql::map_wire_func_t generate_random_field_wire_func() {
    ql::minidriver_t r(ql::backtrace_id_t::empty());
    auto x = ql::minidriver_t::dummy_var_t::SINDEXCREATE_X;
    ql::var_scope_t scope;
    ql::compile_env_t empty_compile_env(scope.compute_visibility());

    ql::datum_t name_datum(rand_string(100));

    counted_t<ql::func_term_t> func_term_term =
        make_counted<ql::func_term_t>(&empty_compile_env,
                                    r.fun(x, r.var(x)[name_datum]).root_term());
    return ql::map_wire_func_t(func_term_term->eval_to_func(scope));
}

bool equivalent_definitions(const sindex_disk_info_t &x, const sindex_disk_info_t &y) {
    // Just check the random wirefuncs by serializing.
    return serialize_for_cluster_to_vector(x.mapping) == serialize_for_cluster_to_vector(y.mapping);
}

TPTEST(BTreeSindex, LowLevelOps) {
    temp_rockstore rocks;

    io_backender_t io_backender(rocks.rocks(), file_direct_io_mode_t::buffered_desired);

    cache_t cache(&get_global_perfmon_collection());
    cache_conn_t cache_conn(&cache);
    namespace_id_t table_id = str_to_uuid("12345678-abcd-abcd-abcd-12345678abcd");

    rockshard rocksh(io_backender.rocks(), table_id);

    {
        txn_t txn(&cache_conn, write_durability_t::HARD, 1);

        auto sb_lock = make_scoped<real_superblock_lock>(&txn, access_t::write, new_semaphore_in_line_t());
        btree_slice_t::init_real_superblock(
            sb_lock.get(), rockshard(io_backender.rocks(), table_id),
            std::vector<char>(), version_t::zero());

        txn.commit(rocksh.rocks, std::move(sb_lock));
    }

    order_source_t order_source;

    std::map<sindex_name_t, secondary_index_t> mirror;

    {
        scoped_ptr_t<txn_t> txn;

        scoped_ptr_t<real_superblock_lock> superblock;
        get_btree_superblock_and_txn_for_writing(&cache_conn, nullptr,
            write_access_t::write, 1,
            write_durability_t::SOFT,
            &superblock, &txn);

        superblock->sindex_block_write_signal()->wait();

        initialize_secondary_indexes(rockshard(io_backender.rocks(), table_id), superblock.get());

        txn->commit(io_backender.rocks(), std::move(superblock));
    }

    for (int i = 0; i < 100; ++i) {
        sindex_name_t name(uuid_to_str(generate_uuid()));

        secondary_index_t s;

        sindex_disk_info_t definition(
            generate_random_field_wire_func(), sindex_reql_version_info_t::LATEST(), 
            sindex_multi_bool_t::SINGLE, sindex_geo_bool_t::REGULAR);
        s.definition = definition;

        mirror[name] = s;

        scoped_ptr_t<txn_t> txn;
        scoped_ptr_t<real_superblock_lock> superblock;
        get_btree_superblock_and_txn_for_writing(
            &cache_conn,
            nullptr,
            write_access_t::write,
            1,
            write_durability_t::SOFT,
            &superblock,
            &txn);

        superblock->sindex_block_write_signal()->wait();

        set_secondary_index(rocksh, superblock.get(), name, s);

        txn->commit(rocksh.rocks, std::move(superblock));
    }

    {
        scoped_ptr_t<txn_t> txn;
        scoped_ptr_t<real_superblock_lock> superblock;
        get_btree_superblock_and_txn_for_writing(
            &cache_conn,
            nullptr,
            write_access_t::write,
            1,
            write_durability_t::SOFT,
            &superblock,
            &txn);

        superblock->sindex_block_write_signal()->wait();

        std::map<sindex_name_t, secondary_index_t> sindexes;
        get_secondary_indexes(rocksh, superblock.get(), &sindexes);

        auto it = sindexes.begin();
        auto jt = mirror.begin();

        for (;;) {
            if (it == sindexes.end()) {
                ASSERT_TRUE(jt == mirror.end());
                break;
            }
            ASSERT_TRUE(jt != mirror.end());

            ASSERT_TRUE(it->first == jt->first);
            ASSERT_TRUE(
                equivalent_definitions(it->second.definition, jt->second.definition));
            ++it;
            ++jt;
        }

        txn->commit(rocksh.rocks, std::move(superblock));
    }
}


} // namespace unittest
