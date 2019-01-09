// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "btree/get_distribution.hpp"

#include "rocksdb/utilities/optimistic_transaction_db.h"
#include "rocksdb/metadata.h"

#include "arch/runtime/thread_pool.hpp"
#include "btree/internal_node.hpp"
#include "btree/node.hpp"
#include "btree/leaf_node.hpp"
#include "btree/parallel_traversal.hpp"
#include "btree/superblock.hpp"
#include "buffer_cache/alt.hpp"
#include "rockstore/store.hpp"
#include "utils.hpp"

class get_distribution_traversal_helper_t : public btree_traversal_helper_t, public home_thread_mixin_debug_only_t {
public:
    get_distribution_traversal_helper_t(int _depth_limit, std::vector<store_key_t> *_keys)
        : depth_limit(_depth_limit), key_count(0), keys(_keys)
    { }

    void read_stat_block(buf_lock_t *stat_block) {
        guarantee (stat_block != nullptr);
        buf_read_t read(stat_block);
        uint32_t sb_size;
        const btree_statblock_t *sb_data =
            static_cast<const btree_statblock_t *>(read.get_data_read(&sb_size));
        guarantee(sb_size == BTREE_STATBLOCK_SIZE);
        key_count = sb_data->population;
    }

    // This is free to call mark_deleted.
    void process_a_leaf(buf_lock_t *leaf_node_buf,
                        const btree_key_t *,
                        const btree_key_t *,
                        signal_t * /*interruptor*/,
                        int * /*population_change_out*/) THROWS_ONLY(interrupted_exc_t) {
        buf_read_t read(leaf_node_buf);
        const leaf_node_t *node
            = static_cast<const leaf_node_t *>(read.get_data_read());

        for (auto it = leaf::begin(*node); it != leaf::end(*node); ++it) {
            const btree_key_t *key = (*it).first;
            keys->push_back(store_key_t(key->size, key->contents));
        }
    }

    void postprocess_internal_node(buf_lock_t *internal_node_buf) {
        buf_read_t read(internal_node_buf);
        const internal_node_t *node
            = static_cast<const internal_node_t *>(read.get_data_read());

        /* Notice, we iterate all but the last pair because the last pair
         * doesn't actually have a key and we're looking for the split points.
         * */
        for (int i = 0; i < (node->npairs - 1); i++) {
            const btree_internal_pair *pair = internal_node::get_pair_by_index(node, i);
            keys->push_back(store_key_t(pair->key.size, pair->key.contents));
        }
    }

    void filter_interesting_children(buf_parent_t,
                                     ranged_block_ids_t *ids_source,
                                     interesting_children_callback_t *cb) {
        if (ids_source->get_level() < depth_limit) {
            int num_block_ids = ids_source->num_block_ids();
            for (int i = 0; i < num_block_ids; ++i) {
                block_id_t block_id;
                const btree_key_t *left, *right;
                ids_source->get_block_id_and_bounding_interval(i, &block_id, &left, &right);

                cb->receive_interesting_child(i);
            }
        } else {
            //We're over the depth limit and thus disinterested in all children
        }

        cb->no_more_interesting_children();
    }

    access_t btree_superblock_mode() {
        return access_t::read;
    }

    access_t btree_node_mode() {
        return access_t::read;
    }

    int depth_limit;
    int64_t key_count;

    //TODO this is inefficient since each one is maximum size
    std::vector<store_key_t> *keys;
};

void get_btree_key_distribution(superblock_t *superblock, int depth_limit,
                                int64_t *key_count_out,
                                std::vector<store_key_t> *keys_out) {
    get_distribution_traversal_helper_t helper(depth_limit, keys_out);
    rassert(keys_out->empty(), "Why is this output parameter not an empty vector\n");

    cond_t non_interruptor;
    btree_parallel_traversal(superblock, &helper, &non_interruptor);
    *key_count_out = helper.key_count;
}

// TODO: Remove unused code above.
// TODO: Remove btree_parallel_traversal :'(


// Outputs a set of "split keys", i.e. keys within key_range (exclusive) that
// break up the region into equal parts.
void get_btree_key_distribution(rockshard rocksh, key_range_t key_range, superblock_t *superblock, int keys_limit,
                                std::vector<store_key_t> *keys_out, std::vector<uint64_t> *counts_out) {
    keys_out->clear();
    rocksdb::OptimisticTransactionDB *db = rocksh.rocks->db();

    size_t num_intervals = keys_limit + 1;

    std::string rocks_kv_prefix = rockstore::table_primary_prefix(rocksh.table_id, rocksh.shard_no);
    std::string beg = rocks_kv_prefix + key_to_unescaped_str(key_range.left);
    std::string end = key_range.right.unbounded
        ? rockstore::prefix_end(rocks_kv_prefix)
        : rocks_kv_prefix + key_to_unescaped_str(key_range.right.key());

    std::vector<store_key_t> ret;
    std::vector<uint64_t> count_output;

    superblock->read_acq_signal()->wait_lazily_unordered();
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
    // TODO: Maybe the caller's job.
    superblock->release();
    *keys_out = std::move(ret);
    *counts_out = std::move(count_output);
}