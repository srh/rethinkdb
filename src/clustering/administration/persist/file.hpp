// Copyright 2010-2015 RethinkDB, all rights reserved.
#ifndef CLUSTERING_ADMINISTRATION_PERSIST_FILE_HPP_
#define CLUSTERING_ADMINISTRATION_PERSIST_FILE_HPP_

#include "rocksdb/write_batch.h"

#include "btree/keys.hpp"
#include "btree/stats.hpp"
#include "buffer_cache/alt.hpp"
#include "buffer_cache/types.hpp"
#include "concurrency/rwlock.hpp"
#include "containers/archive/string_stream.hpp"
#include "rockstore/write_options.hpp"
#include "utils.hpp"

class base_path_t;
class io_backender_t;

class file_in_use_exc_t : public std::exception {
public:
    const char *what() const throw () {
        return "metadata file is being used by another rethinkdb process";
    }
};

namespace metadata {
class read_txn_t;
class write_txn_t;

template<class T>
class key_t {
public:
    explicit key_t(const std::string &s) : key(s) { }
    key_t suffix(const std::string &s) const {
        key_t copy = *this;
        guarantee(key.size() + s.size() <= MAX_KEY_SIZE);
        copy.key.set_size(key.size() + s.size());
        memcpy(copy.key.contents() + key.size(), s.c_str(), s.size());
        return copy;
    }
private:
    friend class metadata_file_t;
    friend class read_txn_t;
    friend class write_txn_t;
    store_key_t key;
};
}

class metadata_file_t {
public:
    template <class T>
    using key_t = metadata::key_t<T>;
    using read_txn_t = metadata::read_txn_t;
    using write_txn_t = metadata::write_txn_t;

    // Used to open an existing metadata file
    metadata_file_t(
        io_backender_t *io_backender,
        perfmon_collection_t *perfmon_parent);

    // Used top create a new metadata file
    metadata_file_t(
        io_backender_t *io_backender,
        perfmon_collection_t *perfmon_parent,
        const std::function<void(write_txn_t *, signal_t *)> &initializer);
    ~metadata_file_t();

private:
    friend class metadata::read_txn_t;
    friend class metadata::write_txn_t;

    rockstore::write_options rocks_options;
    rockstore::store *rocks;
    rwlock_t rwlock;
};


namespace metadata {
    // This read_txn_t/write_txn_t stuff might be kind of obtuse and overengineered with
    // the rocksdb backend -- but we are maintaining compatibility with older callers.
    class read_txn_t {
    public:
        read_txn_t(metadata_file_t *file, signal_t *interruptor);

        template<class T, cluster_version_t W = cluster_version_t::LATEST_DISK>
        T read(const key_t<T> &key) {
            T value;
            bool found = read_maybe<T, W>(key, &value);
            guarantee(found, "failed to find expected metadata key");
            return value;
        }

        template<class T, cluster_version_t W = cluster_version_t::LATEST_DISK>
        bool read_maybe(
                const key_t<T> &key,
                T *value_out) {
            std::pair<std::string, bool> value = read_bin(key.key);
            if (!value.second) {
                return false;
            }
            string_read_stream_t stream(std::move(value.first), 0);
            // TODO: No need for W template param?
            archive_result_t res = deserialize<W>(&stream, value_out);
            guarantee_deserialization(res, "metadata_file_t::read_txn_t::read");
            return true;
        }

        template<class T, cluster_version_t W = cluster_version_t::LATEST_DISK>
        void read_many(
                const key_t<T> &key_prefix,
                const std::function<void(
                    const std::string &key_suffix, const T &value)> &cb,
                signal_t *interruptor) {
            read_many_bin(
                key_prefix.key,
                [&](const std::string &key_suffix, read_stream_t *bin_value) {
                    T value;
                    archive_result_t res = deserialize<W>(bin_value, &value);
                    guarantee_deserialization(res,
                        "metadata_file_t::read_txn_t::read_many");
                    cb(key_suffix, value);
                },
                interruptor);
        }

        friend class ::metadata_file_t;
        friend class write_txn_t;

    private:
        /* This constructor is used by `write_txn_t` */
        read_txn_t(metadata_file_t *file, write_access_t, signal_t *interruptor);

        std::pair<std::string, bool> read_bin(
            const store_key_t &key);

        void read_many_bin(
            const store_key_t &key_prefix,
            const std::function<void(
                const std::string &key_suffix, read_stream_t *)> &cb,
            signal_t *interruptor);

        metadata_file_t *file;
        rwlock_acq_t rwlock_acq;
    };

class write_txn_t : public read_txn_t {
public:
    write_txn_t(metadata_file_t *file, signal_t *interruptor);

    template<class T>
    void write(
            const key_t<T> &key,
            const T &value,
            signal_t *interruptor) {
        write_message_t wm;
        serialize<cluster_version_t::LATEST_DISK>(&wm, value);
        write_bin(key.key, &wm, interruptor);
    }

    template<class T>
    void erase(const key_t<T> &key, signal_t *interruptor) {
        write_bin(key.key, nullptr, interruptor);
    }

    // Must be called before the `write_txn_t` is destructed.
    // This acts as a safety check to make sure a transaction
    // is not interrupted in the middle, which could leave the
    // metadata in an inconsistent state.
    void commit();

private:
    friend class metadata_file_t;

    rocksdb::WriteBatch batch;

    void write_bin(
        const store_key_t &key,
        const write_message_t *msg,
        signal_t *interruptor);
};
}  // namespace metadata

#endif /* CLUSTERING_ADMINISTRATION_PERSIST_FILE_HPP_ */

