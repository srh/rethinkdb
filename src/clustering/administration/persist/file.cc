// Copyright 2010-2015 RethinkDB, all rights reserved.
#include "clustering/administration/persist/file.hpp"

#include "arch/io/disk.hpp"
#include "btree/types.hpp"
#include "config/args.hpp"
#include "logger.hpp"
#include "paths.hpp"
#include "rockstore/store.hpp"
#include "serializer/log/log_serializer.hpp"
#include "serializer/merger.hpp"

// See ROCKSDB_STORAGE_FORMAT documentation
const std::string METADATA_PREFIX = "rethinkdb/metadata/";
const std::string METADATA_VERSION_KEY = "rethinkdb/metadata/version";

metadata_file_t::read_txn_t::read_txn_t(
        metadata_file_t *f,
        signal_t *interruptor) :
    file(f),
    rwlock_acq(&file->rwlock, access_t::read, interruptor)
    { }

metadata_file_t::read_txn_t::read_txn_t(
        metadata_file_t *f,
        write_access_t,
        signal_t *interruptor) :
    file(f),
    rwlock_acq(&file->rwlock, access_t::write, interruptor)
    { }

std::pair<std::string, bool> metadata_file_t::read_txn_t::read_bin(
        const store_key_t &key) {
    return file->rocks->try_read(METADATA_PREFIX + key_to_unescaped_str(key));
}

void metadata_file_t::read_txn_t::read_many_bin(
        const store_key_t &key_prefix,
        const std::function<void(const std::string &key_suffix, read_stream_t *)> &cb,
        signal_t *interruptor) {
    // TODO: Use or remove interruptor.
    (void)interruptor;
    std::string full_prefix = METADATA_PREFIX + key_to_unescaped_str(key_prefix);
    // TODO: Might there be any need to truly stream this?
    std::vector<std::pair<std::string, std::string>> all
        = file->rocks->read_all_prefixed(full_prefix);
    const size_t prefix_size = full_prefix.size();
    for (auto& p : all) {
        guarantee(p.first.size() >= prefix_size);
        guarantee(memcmp(p.first.data(), full_prefix.data(), prefix_size) == 0);
        std::string suffix = p.first.substr(prefix_size);
        string_read_stream_t stream(std::move(p.second), 0);
        cb(suffix, &stream);
    }

    return;
}

metadata_file_t::write_txn_t::write_txn_t(
        metadata_file_t *_file,
        signal_t *interruptor) :
    read_txn_t(_file, write_access_t::write, interruptor)
    { }

void metadata_file_t::write_txn_t::commit() {
    file->rocks->write_batch(std::move(batch), file->rocks_options);
}

void metadata_file_t::write_txn_t::write_bin(
        const store_key_t &key,
        const write_message_t *msg,
        signal_t *interruptor) {
    // TODO: Use or remove interruptor param.
    (void)interruptor;
    // TODO: Verify that we can stack writes and deletes on a rocksdb WriteBatch.
    std::string rockskey = METADATA_PREFIX + key_to_unescaped_str(key);
    if (msg == nullptr) {
        batch.Delete(rockskey);
    } else {
        string_stream_t stream;
        int res = send_write_message(&stream, msg);
        guarantee(res == 0);
        batch.Put(rockskey, stream.str());
    }
}

metadata_file_t::metadata_file_t(
        io_backender_t *io_backender,
        perfmon_collection_t *perfmon_parent) :
    rocks_options(true),
    rocks(io_backender->rocks()) {
    // TODO: Make use of perfmon with rockstore, to track metadata writes.
    (void)perfmon_parent;

    // TODO: Remove unused metadata migration logic.
    std::string metadata_version = rocks->read(METADATA_VERSION_KEY);
    if (metadata_version != rockstore::VERSION()) {
        // TODO
        throw std::runtime_error("Unsupported metadata version");
    }
}

metadata_file_t::metadata_file_t(
        io_backender_t *io_backender,
        perfmon_collection_t *perfmon_parent,
        const std::function<void(write_txn_t *, signal_t *)> &initializer) :
    rocks_options(true),
    rocks(io_backender->rocks())
{
    // TODO: Make use of perfmon with rockstore, to track metadata writes.
    (void)perfmon_parent;

    rocks->insert(METADATA_VERSION_KEY, rockstore::VERSION(), rocks_options);

    {
        cond_t non_interruptor;
        write_txn_t write_txn(this, &non_interruptor);
        initializer(&write_txn, &non_interruptor);
        write_txn.commit();
    }
}

metadata_file_t::~metadata_file_t() { }

