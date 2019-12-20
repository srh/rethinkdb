// Copyright 2010-2015 RethinkDB, all rights reserved.
#include "btree/reql_specific.hpp"

#include "rocksdb/write_batch.h"

#include "rdb_protocol/secondary_operations.hpp"
#include "clustering/immediate_consistency/version.hpp"
#include "rockstore/store.hpp"


void btree_slice_t::init_real_superblock(real_superblock_lock *superblock,
                                         rockshard rocksh,
                                         const std::vector<char> &metainfo_key,
                                         const version_t &metainfo_value) {
    superblock->write_acq_signal()->wait_lazily_ordered();
    set_superblock_metainfo(superblock, rocksh, metainfo_key, metainfo_value);
    initialize_secondary_indexes(rocksh, superblock);
}

btree_slice_t::btree_slice_t(cache_t *c, perfmon_collection_t *parent,
                             const std::string &identifier,
                             index_type_t index_type)
    : stats(parent,
            (index_type == index_type_t::SECONDARY ? "index-" : "") + identifier),
      cache_(c) { }

btree_slice_t::~btree_slice_t() { }

void superblock_metainfo_iterator_t::advance(const char *p) {
    const char *cur = p;
    if (cur == end) {
        goto check_failed;
    }
    rassert(end - cur >= static_cast<ptrdiff_t>(sizeof(sz_t)), "Superblock metainfo data is corrupted: walked past the end off the buffer");
    if (end - cur < static_cast<ptrdiff_t>(sizeof(sz_t))) {
        goto check_failed;
    }
    key_size = *reinterpret_cast<const sz_t *>(cur);
    cur += sizeof(sz_t);

    rassert(end - cur >= static_cast<int64_t>(key_size), "Superblock metainfo data is corrupted: walked past the end off the buffer");
    if (end - cur < static_cast<int64_t>(key_size)) {
        goto check_failed;
    }
    key_ptr = cur;
    cur += key_size;

    rassert(end - cur >= static_cast<ptrdiff_t>(sizeof(sz_t)), "Superblock metainfo data is corrupted: walked past the end off the buffer");
    if (end - cur < static_cast<ptrdiff_t>(sizeof(sz_t))) {
        goto check_failed;
    }
    value_size = *reinterpret_cast<const sz_t *>(cur);
    cur += sizeof(sz_t);

    rassert(end - cur >= static_cast<int64_t>(value_size), "Superblock metainfo data is corrupted: walked past the end off the buffer");
    if (end - cur < static_cast<int64_t>(value_size)) {
        goto check_failed;
    }
    value_ptr = cur;
    cur += value_size;

    pos = p;
    next_pos = cur;

    return;

check_failed:
    pos = next_pos = end;
    key_size = value_size = 0;
    key_ptr = value_ptr = nullptr;
}

void superblock_metainfo_iterator_t::operator++() {
    if (!is_end()) {
        advance(next_pos);
    }
}

void get_superblock_metainfo(
        rockshard rocksh,
        real_superblock_lock *superblock,
        std::vector<std::pair<std::vector<char>, std::vector<char> > > *kv_pairs_out) {
    superblock->read_acq_signal()->wait_lazily_ordered();

    std::string metainfo_key = rockstore::table_metadata_prefix(rocksh.table_id)
        + rockstore::TABLE_METADATA_METAINFO_KEY();
    std::string metainfo = rocksh.rocks->read(metainfo_key);

    for (superblock_metainfo_iterator_t kv_iter(metainfo.data(), metainfo.data() + metainfo.size()); !kv_iter.is_end(); ++kv_iter) {
        superblock_metainfo_iterator_t::key_t key = kv_iter.key();
        superblock_metainfo_iterator_t::value_t value = kv_iter.value();
        kv_pairs_out->push_back(std::make_pair(std::vector<char>(key.second, key.second + key.first), std::vector<char>(value.second, value.second + value.first)));
    }
}

void set_superblock_metainfo(real_superblock_lock *superblock,
                             rockshard rocksh,
                             const std::vector<char> &key,
                             const version_t &value) {
    std::vector<std::vector<char> > keys = {key};
    std::vector<version_t> values = {value};
    set_superblock_metainfo(superblock, rocksh, keys, values);
}

void set_superblock_metainfo(real_superblock_lock *superblock,
                             rockshard rocksh,
                             const std::vector<std::vector<char> > &keys,
                             const std::vector<version_t> &values) {
    // Acquire lock explicitly for rocksdb writing.
    superblock->write_acq_signal()->wait_lazily_ordered();

    std::vector<char> metainfo;

    rassert(keys.size() == values.size());
    auto value_it = values.begin();
    for (auto key_it = keys.begin(); key_it != keys.end(); ++key_it, ++value_it) {
        union {
            char x[sizeof(uint32_t)];
            uint32_t y;
        } u;
        rassert(key_it->size() < UINT32_MAX);

        u.y = key_it->size();
        metainfo.insert(metainfo.end(), u.x, u.x + sizeof(uint32_t));
        metainfo.insert(metainfo.end(), key_it->begin(), key_it->end());

        version_t value = *value_it;
        u.y = sizeof(value);
        metainfo.insert(metainfo.end(), u.x, u.x + sizeof(uint32_t));
        metainfo.insert(
            metainfo.end(),
            reinterpret_cast<const uint8_t *>(&value),
            reinterpret_cast<const uint8_t *>(&value) + sizeof(value));
    }

    // Rocksdb metadata.
    rocksdb::WriteBatch batch;
    std::string meta_prefix = rockstore::table_metadata_prefix(rocksh.table_id);
    rocksdb::Status status = superblock->wait_write_batch()->Put(
        meta_prefix + rockstore::TABLE_METADATA_METAINFO_KEY(),
        rocksdb::Slice(metainfo.data(), metainfo.size()));
    guarantee(status.ok());
}

void get_btree_superblock(
        txn_t *txn,
        access_t access,
        scoped_ptr_t<real_superblock_lock> *got_superblock_out) {
    // TODO: Is access ever not read access?
    *got_superblock_out = make_scoped<real_superblock_lock>(txn, access, new_semaphore_in_line_t());
}

/* Variant for writes that go through a superblock write semaphore */
void get_btree_superblock(
        txn_t *txn,
        UNUSED write_access_t access,
        new_semaphore_in_line_t &&write_sem_acq,
        scoped_ptr_t<real_superblock_lock> *got_superblock_out) {
    *got_superblock_out = make_scoped<real_superblock_lock>(txn, access_t::write, std::move(write_sem_acq));
}

void get_btree_superblock_and_txn_for_writing(
        cache_conn_t *cache_conn,
        new_semaphore_t *superblock_write_semaphore,
        UNUSED write_access_t superblock_access,
        int expected_change_count,
        write_durability_t durability,
        scoped_ptr_t<real_superblock_lock> *got_superblock_out,
        scoped_ptr_t<txn_t> *txn_out) {
    txn_t *txn = new txn_t(cache_conn, durability, expected_change_count);

    txn_out->init(txn);

    /* Acquire a ticket from the superblock_write_semaphore */
    new_semaphore_in_line_t sem_acq;
    if(superblock_write_semaphore != nullptr) {
        sem_acq.init(superblock_write_semaphore, 1);
        sem_acq.acquisition_signal()->wait();
    }

    get_btree_superblock(txn, write_access_t::write, std::move(sem_acq), got_superblock_out);
}

void get_btree_superblock_and_txn_for_backfilling(
        cache_conn_t *cache_conn,
        cache_account_t *backfill_account,
        scoped_ptr_t<real_superblock_lock> *got_superblock_out,
        scoped_ptr_t<txn_t> *txn_out) {
    txn_t *txn = new txn_t(cache_conn, read_access_t::read);
    txn_out->init(txn);
    txn->set_account(backfill_account);

    get_btree_superblock(txn, access_t::read, got_superblock_out);
}

// KSI: This function is possibly stupid: it's nonsensical to talk about the entire
// cache being snapshotted -- we want some subtree to be snapshotted, at least.
// However, if you quickly release the superblock, you'll release any snapshotting of
// secondary index nodes that you could not possibly access.
void get_btree_superblock_and_txn_for_reading(
        cache_conn_t *cache_conn,
        scoped_ptr_t<real_superblock_lock> *got_superblock_out,
        scoped_ptr_t<txn_t> *txn_out) {
    txn_t *txn = new txn_t(cache_conn, read_access_t::read);
    txn_out->init(txn);

    get_btree_superblock(txn, access_t::read, got_superblock_out);
}
