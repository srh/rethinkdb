// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "unittest/gtest.hpp"

#include "arch/io/disk.hpp"
#include "btree/operations.hpp"
#include "btree/reql_specific.hpp"
#include "buffer_cache/alt.hpp"
#include "containers/uuid.hpp"
#include "unittest/unittest_utils.hpp"
#include "random.hpp"
#include "rdb_protocol/btree.hpp"
#include "rdb_protocol/datum.hpp"
#include "rdb_protocol/store.hpp"
#include "rdb_protocol/protocol.hpp"
#include "serializer/log/log_serializer.hpp"

namespace unittest {

TPTEST(BTreeSindex, LowLevelOps) {
    temp_rockstore rocks;

    io_backender_t io_backender(rocks.rocks(), file_direct_io_mode_t::buffered_desired);

    cache_t cache(&get_global_perfmon_collection());
    cache_conn_t cache_conn(&cache);
    namespace_id_t table_id = str_to_uuid("12345678-abcd-abcd-abcd-12345678abcd");

    rockshard rocksh(io_backender.rocks(), table_id, 0);

    {
        txn_t txn(&cache_conn, write_durability_t::HARD, 1);
        {
            real_superblock_lock sb_lock(&txn, access_t::write);
            real_superblock_t superblock(std::move(sb_lock));
            btree_slice_t::init_real_superblock(
                &superblock, rockshard(io_backender.rocks(), table_id, 0), std::vector<char>(), binary_blob_t());
        }
        txn.commit();
    }

    order_source_t order_source;

    std::map<sindex_name_t, secondary_index_t> mirror;

    {
        scoped_ptr_t<txn_t> txn;
        {
            scoped_ptr_t<real_superblock_t> superblock;
            get_btree_superblock_and_txn_for_writing(&cache_conn, nullptr,
                write_access_t::write, 1,
                write_durability_t::SOFT,
                &superblock, &txn);

            sindex_block_lock sindex_block(superblock->get(), access_t::write);

            initialize_secondary_indexes(rockshard(io_backender.rocks(), table_id, 0), &sindex_block);
        }
        txn->commit();
    }

    for (int i = 0; i < 100; ++i) {
        sindex_name_t name(uuid_to_str(generate_uuid()));

        secondary_index_t s;

        std::string opaque_blob = rand_string(1000);
        s.opaque_definition.assign(opaque_blob.begin(), opaque_blob.end());

        mirror[name] = s;

        scoped_ptr_t<txn_t> txn;
        {
            scoped_ptr_t<real_superblock_t> superblock;
            get_btree_superblock_and_txn_for_writing(
                &cache_conn,
                nullptr,
                write_access_t::write,
                1,
                write_durability_t::SOFT,
                &superblock,
                &txn);
            sindex_block_lock sindex_block(
                superblock->get(),
                access_t::write);

            set_secondary_index(rocksh, &sindex_block, name, s);
        }
        txn->commit();
    }

    {
        scoped_ptr_t<txn_t> txn;
        {
            scoped_ptr_t<real_superblock_t> superblock;
            get_btree_superblock_and_txn_for_writing(
                &cache_conn,
                nullptr,
                write_access_t::write,
                1,
                write_durability_t::SOFT,
                &superblock,
                &txn);
            sindex_block_lock sindex_block(
                superblock->get(),
                access_t::write);

            std::map<sindex_name_t, secondary_index_t> sindexes;
            get_secondary_indexes(rocksh, &sindex_block, &sindexes);

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
                    it->second.opaque_definition == jt->second.opaque_definition);
                ++it;
                ++jt;
            }
        }
        txn->commit();
    }
}


} // namespace unittest
