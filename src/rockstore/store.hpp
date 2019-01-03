#ifndef RETHINKDB_ROCKSTORE_STORE_HPP_
#define RETHINKDB_ROCKSTORE_STORE_HPP_

#include <string>
#include <vector>

#include "containers/scoped.hpp"
#include "containers/uuid.hpp"
#include "rockstore/write_options.hpp"

namespace rocksdb {
class OptimisticTransactionDB;
class WriteBatch;
}

class base_path_t;


namespace rockstore {

// TODO: Check all callers for gratuitous std::string construction, use rocksdb::Slice.
// TODO: All the callers of these methods need to be on a transaction (except for metadata).

class store {
public:
    // Throws std::runtime_error.
    std::string read(const std::string &key);
    // Throws std::runtime_error.  False if value not found.
    std::pair<std::string, bool> try_read(const std::string &key);

    std::vector<std::pair<std::string, std::string>> read_all_prefixed(std::string prefix);

    // Overwrites what's there.
    // Throws std::runtime_error.
    void put(const std::string &key, const std::string &value, const write_options &opts);

    // Throws std::runtime_error.
    // TODO: This isn't an atomic op, is it?  Rename this?  Suitable for metadata?
    void insert(const std::string &key, const std::string &value, const write_options &opts);

    // Throws std::runtime_error.
    void remove(const std::string &key, const write_options &opts);

    // Throws std::runtime_error.
    void write_batch(rocksdb::WriteBatch&& batch, const write_options &opts);

    void sync(const write_options &opts);

    ~store();
    store(store&&);
    store& operator=(store&&) = delete;

private:
    explicit store(rocksdb::OptimisticTransactionDB *db);
    friend store create_rockstore(const base_path_t &base_path);
    scoped_ptr_t<rocksdb::OptimisticTransactionDB> db_;
};

// Creates the db's sole global rocksdb store.  Called once in the lifetime of the data
// directory.  Throws std::runtime_error.
store create_rockstore(const base_path_t &base_path);

// TODO: Not inline
inline std::string table_prefix(namespace_id_t id) {
    // TODO: Do we use a binary or non-binary UUID?
    std::string ret = "tables/" + uuid_to_str(id) + "/";
    return ret;
}
inline std::string table_metadata_prefix(namespace_id_t id) {
    std::string ret = table_prefix(id);
    ret += "metadata/";
    return ret;
}

// TODO: Remove primary key length limitation at some point.
// TODO: table_secondary_key is still using gnarly 250-byte sindex keys.

inline std::string table_secondary_key(namespace_id_t id, const std::string &index_name,
                                       const std::string &key) {
    std::string ret = "tables/" + uuid_to_str(id) + "/" + index_name + "/" + key;
    return ret;
}

inline std::string table_primary_key(namespace_id_t id, const std::string &key) {
    // We use the empty index name for primary index.
    return table_secondary_key(id, "", key);
}

inline const char * VERSION() { return "v2_4"; }
inline const char * TABLE_METADATA_VERSION_KEY() { return "version"; }
inline const char * TABLE_METADATA_METAINFO_KEY() { return "metainfo"; }

}  // namespace rockstore

/*
ROCKSDB_STORAGE_FORMAT

Metadata (system metadata file):

rethinkdb/metadata/version => "v2_4"
rethinkdb/metadata/<key> => <value>

Tables

tables/<table id>/metadata/version => "v2_4"
tables/<table id>/metadata/<key> => <value>




*/

#endif  // RETHINKDB_ROCKSTORE_STORE_HPP_
