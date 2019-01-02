#ifndef RETHINKDB_ROCKSTORE_STORE_HPP_
#define RETHINKDB_ROCKSTORE_STORE_HPP_

#include <string>
#include <vector>

#include "containers/scoped.hpp"
#include "containers/uuid.hpp"

namespace rocksdb {
class OptimisticTransactionDB;
class WriteBatch;
}

class base_path_t;


namespace rockstore {

struct write_options {
    write_options() {}
    explicit write_options(bool _sync) : sync(_sync) {}
    bool sync = false;
    static write_options TODO() { return write_options(false); }
};

class store {
public:
    // Throws std::runtime_error.
    std::string read(const std::string &key);
    // Throws std::runtime_error.  False if value not found.
    std::pair<std::string, bool> try_read(const std::string &key);

    std::vector<std::pair<std::string, std::string>> read_all_prefixed(std::string prefix);

    // Throws std::runtime_error.
    void insert(const std::string &key, const std::string &value, const write_options &opts);

    // Throws std::runtime_error.
    void write_batch(rocksdb::WriteBatch&& batch, const write_options &opts);

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
