#ifndef RETHINKDB_ROCKSTORE_ROCKSHARD_HPP_
#define RETHINKDB_ROCKSTORE_ROCKSHARD_HPP_

#include "containers/uuid.hpp"

namespace rockstore { class store; }

// TODO: Optimize usage of this parameter.
class rockshard {
public:
    rockshard(rockstore::store *_rocks, namespace_id_t _table_id, int _shard_no)
        : rocks(_rocks), table_id(_table_id), shard_no(_shard_no) {}
    rockstore::store *rocks;
    namespace_id_t table_id;
    int shard_no;
    rockstore::store *operator->() const { return rocks; }
};

#endif  // RETHINKDB_ROCKSTORE_ROCKSHARD_HPP_
