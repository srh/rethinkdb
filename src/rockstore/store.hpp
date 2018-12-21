#ifndef RETHINKDB_ROCKSTORE_STORE_HPP_
#define RETHINKDB_ROCKSTORE_STORE_HPP_

#include "rocksdb/db.h"

namespace rockstore {

class store {
private:
    rocksdb::DB *db_ = nullptr;
};

// Creates the db's sole global rocksdb store.  Called once in the lifetime of the data
// directory.
store create_rockstore();


}  // namespace rockstore

#endif  // RETHINKDB_ROCKSTORE_STORE_HPP_
