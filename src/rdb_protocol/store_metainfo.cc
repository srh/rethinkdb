// Copyright 2010-2015 RethinkDB, all rights reserved.
#include "rdb_protocol/store_metainfo.hpp"

#include "btree/reql_specific.hpp"
#include "clustering/immediate_consistency/version.hpp"
#include "containers/archive/buffer_stream.hpp"
#include "containers/archive/vector_stream.hpp"
#include "rockstore/rockshard.hpp"

store_metainfo_manager_t::store_metainfo_manager_t(rockshard rocksh, real_superblock_lock *superblock) {
    std::vector<std::pair<std::vector<char>, std::vector<char> > > kv_pairs;
    // TODO: this is inefficient, cut out the middleman (vector)
    get_superblock_metainfo(rocksh, superblock, &kv_pairs);
    std::vector<region_t> regions;
    std::vector<version_t> values;
    for (auto &pair : kv_pairs) {
        region_t region;
        {
            buffer_read_stream_t key(pair.first.data(), pair.first.size());
            archive_result_t res = deserialize_for_metainfo(&key, &region);
            guarantee_deserialization(res, "region");
        }
        regions.push_back(region);
        guarantee(pair.second.size() == sizeof(version_t));
        version_t value = *reinterpret_cast<const version_t *>(pair.second.data());
        values.push_back(value);
    }
    cache = region_map_t<version_t>::from_unordered_fragments(
        std::move(regions), std::move(values));
    rassert(cache.get_domain() == region_t::universe());
}

region_map_t<version_t> store_metainfo_manager_t::get(
        real_superblock_lock *superblock,
        const region_t &region) const {
    guarantee(superblock != nullptr);
    superblock->read_acq_signal()->wait_lazily_ordered();
    return cache.mask(region);
}

void store_metainfo_manager_t::visit(
        real_superblock_lock *superblock,
        const region_t &region,
        const std::function<void(const region_t &, const version_t &)> &cb) const {
    guarantee(superblock != nullptr);
    superblock->read_acq_signal()->wait_lazily_ordered();
    cache.visit(region, cb);
}

void store_metainfo_manager_t::update(
        real_superblock_lock *superblock,
        rockshard rocksh,
        const region_map_t<version_t> &new_values) {
    // TODO: Strip out non-rocks writing (but keep superblock write acquisition waiting).
    guarantee(superblock != nullptr);
    superblock->write_acq_signal()->wait_lazily_ordered();

    cache.update(new_values);

    std::vector<std::vector<char> > keys;
    std::vector<version_t> values;
    cache.visit(region_t::universe(),
        [&](const region_t &region, const version_t &value) {
            vector_stream_t key;
            write_message_t wm;
            serialize_for_metainfo(&wm, region);
            key.reserve(wm.size());
            DEBUG_VAR int res = send_write_message(&key, &wm);
            rassert(!res);

            keys.push_back(std::move(key.vector()));
            values.push_back(value);
        });

    set_superblock_metainfo(superblock, rocksh, keys, values);
}
