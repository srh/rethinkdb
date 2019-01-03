#include "rockstore/store.hpp"

#include "rocksdb/db.h"
#include "rocksdb/utilities/optimistic_transaction_db.h"

#include "arch/runtime/thread_pool.hpp"
#include "paths.hpp"

namespace rockstore {

store::store(rocksdb::OptimisticTransactionDB *db) : db_(db) {}
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
    store ret(db);
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
        throw std::runtime_error("store::read failed");
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
            throw std::runtime_error("store::read failed");
        }
    } else {
        ret.second = true;
    }
    return ret;
}

// TODO: Move this somewhere?
bool starts_with(const std::string& x, const std::string& prefix) {
    return x.size() >= prefix.size() &&
        memcmp(x.data(), prefix.data(), prefix.size()) == 0;
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

rocksdb::WriteOptions to_rocks(const write_options &opts) {
    rocksdb::WriteOptions ret;
    ret.sync = opts.sync;
    return ret;
}

void store::put(const std::string &key, const std::string &value,
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

void store::insert(const std::string &key, const std::string &value,
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
        throw std::runtime_error("store::insert on top of existing key");
    }
    if (!status.ok()) {
        // TODO
        throw std::runtime_error("store::insert failed");
    }
    return;
}

void store::write_batch(rocksdb::WriteBatch&& batch, const write_options &opts) {
    rocksdb::Status status;
    linux_thread_pool_t::run_in_blocker_pool([&]() {
        status = db_->Write(to_rocks(opts), &batch);
    });
    if (!status.ok()) {
        // TODO
        throw std::runtime_error("store::write_batch failed");
    }
    return;
}

void store::sync(const write_options &opts) {
    // TODO: Use opts somehow? (There's no soft durability sync is there?)
    (void)opts;
    rocksdb::Status status;
    linux_thread_pool_t::run_in_blocker_pool([&]() {
        status = db_->SyncWAL();
    });
    if (!status.ok()) {
        // TODO
        throw std::runtime_error("store::write_batch failed");
    }
    return;
}

void store::remove(const std::string &key, const write_options &opts) {
    rocksdb::Status status;
    linux_thread_pool_t::run_in_blocker_pool([&]() {
        status = db_->Delete(to_rocks(opts), key);
    });
    if (!status.ok()) {
        // TODO
        throw std::runtime_error("store::remove failed");
    }
    return;
}


}  // namespace rockstore
