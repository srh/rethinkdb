#ifndef RETHINKDB_ROCKSTORE_STORE_HPP_
#define RETHINKDB_ROCKSTORE_STORE_HPP_

#include <string>
#include <vector>

#include "containers/scoped.hpp"

namespace rocksdb {
class DB;
class WriteBatch;
}

class base_path_t;


namespace rockstore {

struct write_options {
    write_options() {}
    explicit write_options(bool _sync) : sync(_sync) {}
    bool sync = false;
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
    explicit store(rocksdb::DB *db);
    friend store create_rockstore(const base_path_t &base_path);
    scoped_ptr_t<rocksdb::DB> db_;
};

// Creates the db's sole global rocksdb store.  Called once in the lifetime of the data
// directory.  Throws std::runtime_error.
store create_rockstore(const base_path_t &base_path);


}  // namespace rockstore

#endif  // RETHINKDB_ROCKSTORE_STORE_HPP_
