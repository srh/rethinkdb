#ifndef RETHINKDB_ROCKSTORE_ROCKSHARD_HPP_
#define RETHINKDB_ROCKSTORE_ROCKSHARD_HPP_

#include "containers/uuid.hpp"

namespace rockstore { class store; }

// This name is a bit outdated -- this used to have a shard_no field for
// hash-sharding.
class rockshard {
public:
    rockshard(rockstore::store *_rocks, namespace_id_t _table_id)
        : rocks(_rocks), table_id(_table_id) {}
    rockstore::store *rocks;
    namespace_id_t table_id;
};

#endif  // RETHINKDB_ROCKSTORE_ROCKSHARD_HPP_
