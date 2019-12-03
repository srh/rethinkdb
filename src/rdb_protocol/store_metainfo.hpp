// Copyright 2010-2015 RethinkDB, all rights reserved.
#ifndef RDB_PROTOCOL_STORE_METAINFO_HPP_
#define RDB_PROTOCOL_STORE_METAINFO_HPP_

#include <functional>

#include "clustering/immediate_consistency/version.hpp"
#include "containers/uuid.hpp"
#include "region/region_map.hpp"

class real_superblock_lock;
class rockshard;

class store_metainfo_manager_t {
public:
    explicit store_metainfo_manager_t(rockshard rocksh, real_superblock_lock *superblock);

    region_map_t<version_t> get(
        real_superblock_lock *superblock,
        const region_t &region) const;

    void visit(
        real_superblock_lock *superblock,
        const region_t &region,
        const std::function<void(const region_t &, const version_t &)> &cb) const;

    void update(
        real_superblock_lock *superblock,
        rockshard rocksh,
        const region_map_t<version_t> &new_metainfo);

private:
    region_map_t<version_t> cache;
};

#endif /* RDB_PROTOCOL_STORE_METAINFO_HPP_ */

