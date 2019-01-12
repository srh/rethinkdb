// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "btree/operations.hpp"

#include <stdint.h>

#include "btree/internal_node.hpp"
#include "btree/leaf_node.hpp"
#include "buffer_cache/alt.hpp"
#include "buffer_cache/blob.hpp"
#include "containers/archive/vector_stream.hpp"
#include "rdb_protocol/profile.hpp"
#include "rdb_protocol/store.hpp"

void insert_root(block_id_t root_id, superblock_t* sb) {
    sb->set_root_block_id(root_id);
}

block_id_t create_stat_block(buf_parent_t parent) {
    buf_lock_t stats_block(parent, alt_create_t::create);
    buf_write_t write(&stats_block);
    // Make the stat block be the default constructed stats block.
    *static_cast<btree_statblock_t *>(write.get_data_write(BTREE_STATBLOCK_SIZE))
        = btree_statblock_t();
    return stats_block.block_id();
}

buf_lock_t get_root(value_sizer_t *sizer, superblock_t *sb) {
    const block_id_t node_id = sb->get_root_block_id();

    if (node_id != NULL_BLOCK_ID) {
        return buf_lock_t(sb->expose_buf(), node_id, access_t::write);
    } else {
        buf_lock_t lock(sb->expose_buf(), alt_create_t::create);
        {
            buf_write_t write(&lock);
            leaf::init(sizer, static_cast<leaf_node_t *>(write.get_data_write()));
        }
        insert_root(lock.block_id(), sb);
        return lock;
    }
}

// Helper function for `check_and_handle_split()` and `check_and_handle_underfull()`.
// Detaches all values in the given node if it's an internal node, and calls
// `detacher` on each value if it's a leaf node.
void detach_all_children(const node_t *node, buf_parent_t parent,
                         const value_deleter_t *detacher) {
    if (node::is_leaf(node)) {
        const leaf_node_t *leaf = reinterpret_cast<const leaf_node_t *>(node);
        // Detach the values that are now in `rbuf` with `buf` as their parent.
        for (auto it = leaf::begin(*leaf); it != leaf::end(*leaf); ++it) {
            detacher->delete_value(parent, (*it).second);
        }
    } else {
        const internal_node_t *internal =
            reinterpret_cast<const internal_node_t *>(node);
        // Detach the values that are now in `rbuf` with `buf` as their parent.
        for (int pair_idx = 0; pair_idx < internal->npairs; ++pair_idx) {
            block_id_t child_id =
                internal_node::get_pair_by_index(internal, pair_idx)->lnode;
            parent.detach_child(child_id);
        }
    }
}

// Split the node if necessary. If the node is a leaf_node, provide the new
// value that will be inserted; if it's an internal node, provide NULL (we
// split internal nodes proactively).
// `detacher` is used to detach any values that are removed from `buf`, in
// case `buf` is a leaf.
void check_and_handle_split(value_sizer_t *sizer,
                            buf_lock_t *buf,
                            buf_lock_t *last_buf,
                            superblock_t *sb,
                            const btree_key_t *key, void *new_value,
                            const value_deleter_t *detacher) {
    {
        buf_read_t buf_read(buf);
        const node_t *node = static_cast<const node_t *>(buf_read.get_data_read());

        // If the node isn't full, we don't need to split, so we're done.
        if (!node::is_internal(node)) { // This should only be called when update_needed.
            rassert(new_value);
            if (!leaf::is_full(sizer, reinterpret_cast<const leaf_node_t *>(node),
                               key, new_value)) {
                return;
            }
        } else {
            rassert(!new_value);
            if (!internal_node::is_full(reinterpret_cast<const internal_node_t *>(node))) {
                return;
            }
        }
    }

    // If we are splitting the root, we must detach it from sb first.
    // It will later be attached to a newly created root, together with its
    // newly created sibling.
    if (last_buf->empty()) {
        sb->expose_buf().detach_child(buf->block_id());
    }

    // Allocate a new node to split into, and some temporary memory to keep
    // track of the median key in the split; then actually split.
    buf_lock_t rbuf(last_buf->empty() ? sb->expose_buf() : buf_parent_t(last_buf),
                    alt_create_t::create);
    store_key_t median_buffer;
    btree_key_t *median = median_buffer.btree_key();

    {
        buf_write_t buf_write(buf);
        buf_write_t rbuf_write(&rbuf);
        node::split(sizer,
                    static_cast<node_t *>(buf_write.get_data_write()),
                    static_cast<node_t *>(rbuf_write.get_data_write()),
                    median);

        // We must detach all entries that we have removed from `buf`.
        buf_read_t rbuf_read(&rbuf);
        const node_t *node = static_cast<const node_t *>(rbuf_read.get_data_read());
        // The parent of the entries used to be `buf`, even though they are now in
        // `rbuf`...
        detach_all_children(node, buf_parent_t(buf), detacher);
    }

    // Since we moved subtrees from `buf` to `rbuf`, we need to set `rbuf`'s recency
    // greater than that of any of its subtrees. We know that `buf`'s recency is greater
    // than that of any of the subtrees that were moved, so we can just copy `buf`'s
    // recency onto `rbuf`. This is more conservative than it needs to be.
    rbuf.set_recency(buf->get_recency());

    // Insert the key that sets the two nodes apart into the parent.
    if (last_buf->empty()) {
        // We're splitting what was previously the root, so create a new root to use as the parent.
        *last_buf = buf_lock_t(sb->expose_buf(), alt_create_t::create);
        {
            buf_write_t last_write(last_buf);
            internal_node::init(sizer->block_size(),
                                static_cast<internal_node_t *>(last_write.get_data_write()));
        }
        // We set the recency of the new root block to the recency of its two sub-trees.
        last_buf->set_recency(buf->get_recency());

        insert_root(last_buf->block_id(), sb);
    }

    {
        buf_write_t last_write(last_buf);
        DEBUG_VAR bool success
            = internal_node::insert(static_cast<internal_node_t *>(last_write.get_data_write()),
                                    median,
                                    buf->block_id(), rbuf.block_id());
        rassert(success, "could not insert internal btree node");
    }

    // We've split the node; now figure out where the key goes and release the other buf (since we're done with it).
    if (0 >= btree_key_cmp(key, median)) {
        // The key goes in the old buf (the left one).

        // Do nothing.

    } else {
        // The key goes in the new buf (the right one).
        buf->swap(rbuf);
    }
}

// Merge or level the node if necessary.
// `detacher` is used to detach any values that are removed from `buf` or its
// sibling, in case `buf` is a leaf.
void check_and_handle_underfull(value_sizer_t *sizer,
                                buf_lock_t *buf,
                                buf_lock_t *last_buf,
                                superblock_t *sb,
                                const btree_key_t *key,
                                const value_deleter_t *detacher) {
    bool node_is_underfull;
    {
        if (last_buf->empty()) {
            // The root node is never underfull.
            node_is_underfull = false;
        } else {
            buf_read_t buf_read(buf);
            const node_t *const node = static_cast<const node_t *>(buf_read.get_data_read());
            node_is_underfull = node::is_underfull(sizer, node);
        }
    }
    if (node_is_underfull) {
        // Acquire a sibling to merge or level with.
        store_key_t key_in_middle;
        block_id_t sib_node_id;
        int nodecmp_node_with_sib;

        {
            buf_read_t last_buf_read(last_buf);
            const internal_node_t *parent_node
                = static_cast<const internal_node_t *>(last_buf_read.get_data_read());
            nodecmp_node_with_sib = internal_node::sibling(parent_node, key,
                                                           &sib_node_id,
                                                           &key_in_middle);
        }

        // Now decide whether to merge or level.
        buf_lock_t sib_buf(last_buf, sib_node_id, access_t::write);

        bool node_is_mergable;
        {
            buf_read_t sib_buf_read(&sib_buf);
            const node_t *sib_node
                = static_cast<const node_t *>(sib_buf_read.get_data_read());

#ifndef NDEBUG
            node::validate(sizer, sib_node);
#endif

            buf_read_t buf_read(buf);
            const node_t *const node
                = static_cast<const node_t *>(buf_read.get_data_read());
            buf_read_t last_buf_read(last_buf);
            const internal_node_t *parent_node
                = static_cast<const internal_node_t *>(last_buf_read.get_data_read());

            node_is_mergable = node::is_mergable(sizer, node, sib_node, parent_node);
        }

        if (node_is_mergable) {
            // Merge.

            const repli_timestamp_t buf_recency = buf->get_recency();
            const repli_timestamp_t sib_buf_recency = sib_buf.get_recency();

            // Nodes must be passed to merge in ascending order.
            // Make it such that we always merge from sib_buf into buf. It
            // simplifies the code below.
            if (nodecmp_node_with_sib < 0) {
                buf->swap(sib_buf);
            }

            bool parent_was_doubleton;
            {
                buf_read_t last_buf_read(last_buf);
                const internal_node_t *parent_node
                    = static_cast<const internal_node_t *>(last_buf_read.get_data_read());
                parent_was_doubleton = internal_node::is_doubleton(parent_node);
            }
            if (parent_was_doubleton) {
                // `buf` will get a new parent below. Detach it from its old one.
                // We can't detach it later because its value will already have
                // been changed by then.
                // And I guess that would be bad, wouldn't it?
                last_buf->detach_child(buf->block_id());
            }

            {
                buf_write_t sib_buf_write(&sib_buf);
                buf_write_t buf_write(buf);
                buf_read_t last_buf_read(last_buf);

                // Detach all values / children in `sib_buf`
                buf_read_t sib_buf_read(&sib_buf);
                const node_t *node =
                    static_cast<const node_t *>(sib_buf_read.get_data_read());
                detach_all_children(node, buf_parent_t(&sib_buf), detacher);

                const internal_node_t *parent_node
                    = static_cast<const internal_node_t *>(last_buf_read.get_data_read());
                node::merge(sizer,
                            static_cast<node_t *>(sib_buf_write.get_data_write()),
                            static_cast<node_t *>(buf_write.get_data_write()),
                            parent_node);
            }
            sib_buf.mark_deleted();
            sib_buf.reset_buf_lock();

            /* `buf` now has sub-trees that came from both `buf` and `sib_buf`. We need
            to set its recency greater than or equal to any of its new sub-trees. We know
            that `buf`'s and `sib_buf`'s old recencies were greater than or equal to
            those of their old sub-trees, so the greater of their recencies must be
            greater than or equal to that of any sub-tree now in `buf`. This is more
            conservative than it needs to be. */
            buf->set_recency(superceding_recency(buf_recency, sib_buf_recency));

            if (!parent_was_doubleton) {
                buf_write_t last_buf_write(last_buf);
                internal_node::remove(sizer->block_size(),
                                      static_cast<internal_node_t *>(last_buf_write.get_data_write()),
                                      key_in_middle.btree_key());
            } else {
                // The parent has only 1 key after the merge (which means that
                // it's the root and our node is its only child). Insert our
                // node as the new root.
                // This is why we had detached `buf` from `last_buf` earlier.
                last_buf->mark_deleted();
                insert_root(buf->block_id(), sb);
            }
        } else {
            // Level.
            store_key_t replacement_key_buffer;
            btree_key_t *replacement_key = replacement_key_buffer.btree_key();

            bool leveled;
            {
                bool is_internal;
                {
                    buf_read_t buf_read(buf);
                    const node_t *node =
                        static_cast<const node_t *>(buf_read.get_data_read());
                    is_internal = node::is_internal(node);
                }
                buf_write_t buf_write(buf);
                buf_write_t sib_buf_write(&sib_buf);
                buf_read_t last_buf_read(last_buf);
                const internal_node_t *parent_node
                    = static_cast<const internal_node_t *>(last_buf_read.get_data_read());
                // We handle internal and leaf nodes separately because of the
                // different ways their children have to be detached.
                // (for internal nodes: call detach_child directly vs. for leaf
                //  nodes: use the supplied value_deleter_t (which might either
                //  detach the children as well or do nothing))
                if (is_internal) {
                    std::vector<block_id_t> moved_children;
                    leveled = internal_node::level(sizer->block_size(),
                            static_cast<internal_node_t *>(buf_write.get_data_write()),
                            static_cast<internal_node_t *>(sib_buf_write.get_data_write()),
                            replacement_key, parent_node, &moved_children);
                    // Detach children that have been removed from `sib_buf`:
                    for (size_t i = 0; i < moved_children.size(); ++i) {
                        sib_buf.detach_child(moved_children[i]);
                    }
                } else {
                    std::vector<const void *> moved_values;
                    leveled = leaf::level(sizer, nodecmp_node_with_sib,
                            static_cast<leaf_node_t *>(buf_write.get_data_write()),
                            static_cast<leaf_node_t *>(sib_buf_write.get_data_write()),
                            replacement_key, &moved_values);
                    // Detach values that have been removed from `sib_buf`:
                    for (size_t i = 0; i < moved_values.size(); ++i) {
                        detacher->delete_value(buf_parent_t(&sib_buf),
                                               moved_values[i]);
                    }
                }
            }

            // We moved new subtrees or values into `buf`, so its recency may need to
            // be increased. Conservatively update it to the max of its old recency and
            // `sib_buf`'s recency, because `sib_buf`'s recency is known to be greater
            // than or equal to the recency of any of the moved subtrees or values.
            buf->set_recency(superceding_recency(
                sib_buf.get_recency(), buf->get_recency()));

            if (leveled) {
                buf_write_t last_buf_write(last_buf);
                internal_node::update_key(static_cast<internal_node_t *>(last_buf_write.get_data_write()),
                                          key_in_middle.btree_key(),
                                          replacement_key);
            }
        }
    }
}
