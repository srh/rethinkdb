#include "rockstore/store.hpp"

#include "rocksdb/db.h"
#include "rocksdb/utilities/transaction.h"
#include "rocksdb/utilities/optimistic_transaction_db.h"

#include "arch/runtime/thread_pool.hpp"
#include "paths.hpp"
#include "utils.hpp"

// TODO: Ctrl+F rocksdb::ReadOptions() and fix usage everywhere.

namespace rockstore {

// TODO: Remove rockstore::txn (if we don't make use of it).

// TODO: Move this somewhere?
bool starts_with(const std::string& x, const std::string& prefix) {
    return x.size() >= prefix.size() &&
        memcmp(x.data(), prefix.data(), prefix.size()) == 0;
}

rocksdb::WriteOptions to_rocks(const write_options &opts) {
    rocksdb::WriteOptions ret;
    ret.sync = opts.sync;
    return ret;
}


snapshot make_snapshot(store *rocks) {
    auto db = rocks->db();
    snapshot ret(db, db->GetSnapshot());
    return ret;
}

snapshot::~snapshot() {
    reset();
}

void snapshot::reset() {
    if (db != nullptr) {
        db->ReleaseSnapshot(snap);
        db = nullptr;
        snap = nullptr;
    }
}


store::store(scoped_ptr_t<rocksdb::OptimisticTransactionDB> &&db) : db_(std::move(db)) {}
store::store(store&& other) : db_(std::move(other.db_)) {}

store create_rockstore(const base_path_t &base_path) {
    // TODO: WAL recovery modes config.
    std::string rocks_path = base_path.path() + "/rockstore";
    rocksdb::OptimisticTransactionDB *db;
    rocksdb::Status status;
    linux_thread_pool_t::run_in_blocker_pool([&]() {
        rocksdb::Options options;
        options.create_if_missing = true;
        status = rocksdb::OptimisticTransactionDB::Open(options, rocks_path, &db);
    });
    if (!status.ok()) {
        // TODO
        throw std::runtime_error("Could not create rockstore");
    }
    store ret{scoped_ptr_t<rocksdb::OptimisticTransactionDB>(db)};
    return ret;
}

store::~store() {
    if (db_.has()) {
        // TODO: At least log the status codes or something.
        db_->SyncWAL();
        db_->Close();
    }
}

std::string store::read(const std::string &key) {
    std::string ret;
    rocksdb::Status status;
    linux_thread_pool_t::run_in_blocker_pool([&]() {
        status = db_->Get(rocksdb::ReadOptions(), key, &ret);
    });
    if (!status.ok()) {
        // TODO
        throw std::runtime_error("store::read failed (on key '" + key + "')");
    }
    return ret;
}

std::pair<std::string, bool> store::try_read(const std::string &key) {
    std::pair<std::string, bool> ret;
    rocksdb::Status status;
    linux_thread_pool_t::run_in_blocker_pool([&]() {
        status = db_->Get(rocksdb::ReadOptions(), key, &ret.first);
    });
    if (!status.ok()) {
        if (status.IsNotFound()) {
            ret.second = false;
        } else {
            // TODO
            throw std::runtime_error("store::read failed (on key '" + key + "')");
        }
    } else {
        ret.second = true;
    }
    return ret;
}

std::vector<std::pair<std::string, std::string>>
store::read_all_prefixed(std::string prefix) {
    std::vector<std::pair<std::string, std::string>> ret;
    linux_thread_pool_t::run_in_blocker_pool([&]() {
        scoped_ptr_t<rocksdb::Iterator> iter(db_->NewIterator(rocksdb::ReadOptions()));
        iter->Seek(prefix);
        while (iter->Valid()) {
            std::string key = iter->key().ToString();
            if (!starts_with(key, prefix)) {
                break;
            }
            std::string value = iter->value().ToString();
            ret.emplace_back(std::move(key), std::move(value));
            iter->Next();
        }
    });
    return ret;
}


void store::deprecated_put(const std::string &key, const std::string &value,
                const write_options &opts) {
    rocksdb::Status status;
    linux_thread_pool_t::run_in_blocker_pool([&]() {
        status = db_->Put(to_rocks(opts), key, value);
    });
    if (!status.ok()) {
        // TODO
        throw std::runtime_error("store::put failed");
    }
    return;
}

void store::deprecated_insert(const std::string &key, const std::string &value,
                   const write_options &opts) {
    rocksdb::Status status;
    bool existed = false;
    linux_thread_pool_t::run_in_blocker_pool([&]() {
        // TODO: Use KeyMayExist.
        std::string old;
        status = db_->Get(rocksdb::ReadOptions(), key, &old);
        if (status.IsNotFound()) {
            status = db_->Put(to_rocks(opts), key, value);
        } else if (status.ok()) {
            existed = true;
        }
    });
    if (existed) {
        // TODO
        throw std::runtime_error("store::insert on top of existing key ('" + key + "')");
    }
    if (!status.ok()) {
        // TODO
        throw std::runtime_error("store::insert failed (on key '" + key + "')");
    }
    return;
}

void store::write_batch(rocksdb::WriteBatch *batch, const write_options &opts) {
    rocksdb::Status status;
    linux_thread_pool_t::run_in_blocker_pool([&]() {
        status = db_->Write(to_rocks(opts), batch);
    });
    if (!status.ok()) {
        // TODO
        throw std::runtime_error("store::write_batch failed");
    }
    return;
}

void store::sync() {
    rocksdb::Status status;
    linux_thread_pool_t::run_in_blocker_pool([&]() {
        status = db_->SyncWAL();
    });
    if (!status.ok()) {
        // TODO
        throw std::runtime_error("store::sync failed");
    }
    return;
}

std::string table_prefix(namespace_id_t id) {
    // TODO: Do we use a binary or non-binary UUID?
    std::string ret = "tables/";
    ret += uuid_to_str(id);
    ret += '/';
    return ret;
}
std::string table_existence_key(namespace_id_t id) {
    std::string ret = table_prefix(id);
    ret += "exists";
    return ret;
}


std::string table_metadata_prefix(namespace_id_t id) {
    std::string ret = table_prefix(id);
    ret += "metadata/";
    return ret;
}
std::string table_sindex_map(namespace_id_t id) {
    std::string ret = table_metadata_prefix(id);
    ret += "sindex_map";
    return ret;
}

// TODO: Remove primary key length limitation at some point.
// TODO: table_secondary_key is still using gnarly 250-byte sindex keys.

std::string table_secondary_prefix(
        namespace_id_t id, uuid_u index_id) {
    std::string ret = table_prefix(id);
    ret += uuid_to_str(index_id);
    ret += '/';
    return ret;
}

std::string table_secondary_key(
        namespace_id_t id, uuid_u index_id,
        const std::string &key) {
    std::string ret = table_secondary_prefix(id, index_id);
    ret += key;
    return ret;
}

std::string table_primary_prefix(namespace_id_t id) {
    std::string ret = table_prefix(id);
    // We use the empty index name for primary index.
    ret += '/';
    return ret;
}

std::string table_primary_key(namespace_id_t id, const std::string &key) {
    std::string ret = table_primary_prefix(id);
    ret += key;
    return ret;
}

// Returns the minimum upper bound of the set of strings prefixed by prefix. If
// and only if there is no upper bound (the string matches /^(\xFF)*$/), returns
// the empty string.
std::string prefix_end(const std::string &prefix) {
    std::string ret = prefix;
    while (!ret.empty()) {
        if (static_cast<uint8_t>(ret.back()) != 0xFF) {
            ret.back() = static_cast<char>(static_cast<uint8_t>(ret.back()) + 1);
            break;
        }
        ret.pop_back();
        continue;
    }
    return ret;
}

// TODO: Use PinnableSlice.  Configure (unconfigure) VerifyChecksums.

}  // namespace rockstore
