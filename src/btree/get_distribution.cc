// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "btree/get_distribution.hpp"

#include "rocksdb/utilities/optimistic_transaction_db.h"
#include "rocksdb/metadata.h"

#include "arch/runtime/thread_pool.hpp"
#include "btree/operations.hpp"
#include "btree/reql_specific.hpp"
#include "rockstore/store.hpp"
#include "utils.hpp"

// Outputs a set of "split keys", i.e. keys within key_range (exclusive) that
// break up the region into equal parts.
void get_distribution(
        rockshard rocksh, key_range_t key_range, int keys_limit,
        std::vector<store_key_t> *keys_out, std::vector<uint64_t> *counts_out) {
    keys_out->clear();
    rocksdb::OptimisticTransactionDB *db = rocksh.rocks->db();

    size_t num_intervals = keys_limit + 1;

    std::string rocks_kv_prefix = rockstore::table_primary_prefix(rocksh.table_id);
    std::string beg = rocks_kv_prefix + key_to_unescaped_str(key_range.left);
    std::string end = key_range.right.unbounded
        ? rockstore::prefix_end(rocks_kv_prefix)
        : rocks_kv_prefix + key_to_unescaped_str(key_range.right.key());

    std::vector<store_key_t> ret;
    std::vector<uint64_t> count_output;

    linux_thread_pool_t::run_in_blocker_pool([&]() {
        std::vector<std::string> key_names;

        rocksdb::ColumnFamilyMetaData meta;
        db->GetBaseDB()->GetColumnFamilyMetaData(&meta);
        for (const rocksdb::LevelMetaData &level_md : meta.levels) {
            for (const rocksdb::SstFileMetaData& file_md : level_md.files) {
                if (file_md.smallestkey < end && file_md.largestkey >= beg) {
                    // We've got overlap.
                    if (file_md.smallestkey >= beg) {
                        key_names.push_back(file_md.smallestkey);
                    }
                }
            }
        }

        key_names.push_back(beg);
        key_names.push_back(end);
        std::sort(key_names.begin(), key_names.end());
        key_names.erase(std::unique(key_names.begin(), key_names.end()), key_names.end());
        key_names.pop_back();

        // At this point, we've got a bunch of key names, in order.
        // Now we're going to go and add up their sizes... approximately.
        std::vector<uint64_t> sizes(key_names.size());

        uint64_t total_size = 0;
        for (const rocksdb::LevelMetaData &level_md : meta.levels) {
            for (const rocksdb::SstFileMetaData& file_md : level_md.files) {
                if (file_md.smallestkey < end && file_md.largestkey >= beg) {
                    // We've got overlap.
                    size_t low = std::lower_bound(key_names.begin(), key_names.end(), file_md.smallestkey) - key_names.begin();
                    size_t high = std::upper_bound(key_names.begin(), key_names.end(), file_md.largestkey) - key_names.begin();
                    size_t diff = high - low;
                    if (diff > 0) {
                        // Apply size to range.  (Don't sweat the remainder.)
                        uint64_t partial_size = file_md.size / diff;
                        for (size_t i = low; i < high; ++i) {
                            sizes.at(i) += partial_size;
                        }
                        total_size += partial_size * diff;
                    }
                }
            }
        }

        // TODO: This just destroys information, should we really do that?
        uint64_t fraction = std::max<uint64_t>(1, total_size / num_intervals);

        std::vector<std::string> output;
        output.push_back(beg);
        uint64_t acc = 0;
        for (size_t i = 0; i < key_names.size(); ++i) {
            if (acc >= fraction) {
                output.push_back(key_names[i]);
                count_output.push_back(acc);
                acc -= fraction;
            }
            acc += sizes[i];
        }
        count_output.push_back(acc);

        for (const std::string &s : output) {
            ret.push_back(store_key_t(s.substr(rocks_kv_prefix.size())));
        }
    });
    *keys_out = std::move(ret);
    *counts_out = std::move(count_output);
}