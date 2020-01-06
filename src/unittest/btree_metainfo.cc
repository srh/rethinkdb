// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "unittest/gtest.hpp"

#include "arch/io/disk.hpp"
#include "btree/operations.hpp"
#include "btree/reql_specific.hpp"
#include "buffer_cache/page_cache.hpp"
#include "clustering/immediate_consistency/version.hpp"
#include "unittest/unittest_utils.hpp"

namespace unittest {

#ifdef _MSC_VER
int (&random)() = rand;
#endif

#ifdef __clang__
#pragma clang diagnostic ignored "-Wunreachable-code"
#endif

std::string random_string() {
    std::string s;
    int length = random() % 10;
    if (random() % 100 == 0) {
        length *= 5000;
    }
    char c = 'a' + random() % ('z' - 'a' + 1);
    return std::string(length, c);
}

std::vector<char> string_to_vector(const std::string &s) {
    return std::vector<char>(s.data(), s.data() + s.size());
}

std::string vector_to_string(const std::vector<char> &v) {
    return std::string(v.data(), v.size());
}

TPTEST(BtreeMetainfo, MetainfoTest) {
    temp_rockstore rocks;
    io_backender_t io_backender(rocks.rocks(), file_direct_io_mode_t::buffered_desired);

    cache_t cache(&get_global_perfmon_collection());
    cache_conn_t cache_conn(&cache);
    namespace_id_t table_id = str_to_uuid("12345678-1234-5678-abcd-12345678abcd");
    rockshard rocksh(io_backender.rocks(), table_id);

    {
        txn_t txn(&cache_conn, write_durability_t::HARD, 1);

        auto sb_lock = make_scoped<real_superblock_lock>(&txn, access_t::write, new_semaphore_in_line_t());
        btree_slice_t::init_real_superblock(
            sb_lock.get(),
            rocksh,
            std::vector<char>());

        txn.commit(rocksh.rocks, std::move(sb_lock));
    }

    for (int i = 0; i < 3; ++i) {
        std::map<std::string, version_t> metainfo;
        for (int j = 0; j < 100; ++j) {
            metainfo[random_string()] = version_t(generate_uuid(), state_timestamp_t::zero());
        }
        {
            scoped_ptr_t<txn_t> txn;

            scoped_ptr_t<real_superblock_lock> superblock;
            get_btree_superblock_and_txn_for_writing(
                &cache_conn, nullptr, write_access_t::write, 1,
                write_durability_t::SOFT, &superblock, &txn);
            std::vector<std::vector<char> > keys;
            std::vector<version_t> values;
            for (const auto &pair : metainfo) {
                keys.push_back(string_to_vector(pair.first));
                values.push_back(pair.second);
            }
            set_superblock_metainfo(superblock.get(), rocksh, keys, values);

            txn->commit(rocksh.rocks, std::move(superblock));
        }
        {
            scoped_ptr_t<txn_t> txn;
            scoped_ptr_t<real_superblock_lock> superblock;
            get_btree_superblock_and_txn_for_reading(
                &cache_conn, &superblock, &txn);
            std::vector<std::pair<std::vector<char>, std::vector<char> > > read_back;
            get_superblock_metainfo(rocksh, superblock.get(), &read_back);
            std::set<std::string> seen;
            for (const auto &pair : read_back) {
                std::string key = vector_to_string(pair.first);
                ASSERT_EQ(sizeof(version_t), pair.second.size());
                version_t value = *reinterpret_cast<const version_t *>(pair.second.data());
                ASSERT_EQ(0, seen.count(key));
                seen.insert(key);
                ASSERT_EQ(metainfo[key], value);
            }
            ASSERT_EQ(metainfo.size(), seen.size());
        }
    }
}

}   /* namespace unittest */
