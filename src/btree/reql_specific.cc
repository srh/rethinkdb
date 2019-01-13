// Copyright 2010-2015 RethinkDB, all rights reserved.
#include "btree/reql_specific.hpp"

#include "rocksdb/write_batch.h"

#include "btree/secondary_operations.hpp"
#include "containers/binary_blob.hpp"
#include "rockstore/store.hpp"


// TODO: Remove this struct (reql_btree_superblock_t) entirely.
/* This is the actual structure stored on disk for the superblock of a table's primary or
sindex B-tree. Both of them use the exact same format, but the sindex B-trees don't make
use of the `sindex_block` or `metainfo_blob` fields. */
ATTR_PACKED(struct reql_btree_superblock_t {
    block_magic_t magic;
    block_id_t root_block;
    block_id_t stat_block_unused;
    block_id_t sindex_block_unused;

    static const int METAINFO_BLOB_MAXREFLEN
        = from_ser_block_size_t<DEVICE_BLOCK_SIZE>::cache_size - sizeof(block_magic_t)
                                                               - 3 * sizeof(block_id_t);

    char metainfo_blob_unused[METAINFO_BLOB_MAXREFLEN];
});

void btree_superblock_ct_asserts() {
    // Just some place to put the CT_ASSERTs
    CT_ASSERT(reql_btree_superblock_t::METAINFO_BLOB_MAXREFLEN > 0);
    CT_ASSERT(from_cache_block_size_t<sizeof(reql_btree_superblock_t)>::ser_size
              == DEVICE_BLOCK_SIZE);
}


real_superblock_t::real_superblock_t(real_superblock_lock &&sb_buf)
    : sb_buf_(std::move(sb_buf)) {}

real_superblock_t::real_superblock_t(
        real_superblock_lock &&sb_buf,
        new_semaphore_in_line_t &&write_semaphore_acq)
    : write_semaphore_acq_(std::move(write_semaphore_acq)),
      sb_buf_(std::move(sb_buf)) {}

void real_superblock_t::release() {
    sb_buf_.reset_buf_lock();
    write_semaphore_acq_.reset();
}

const signal_t *real_superblock_t::read_acq_signal() {
    return sb_buf_.read_acq_signal();
}

const signal_t *real_superblock_t::write_acq_signal() {
    return sb_buf_.write_acq_signal();
}

sindex_superblock_t::sindex_superblock_t(sindex_superblock_lock &&sb_buf)
    : sb_buf_(std::move(sb_buf)) {}

void sindex_superblock_t::release() {
    sb_buf_.reset_buf_lock();
}

const signal_t *sindex_superblock_t::read_acq_signal() {
    return sb_buf_.read_acq_signal();
}

const signal_t *sindex_superblock_t::write_acq_signal() {
    return sb_buf_.write_acq_signal();
}

// TODO: Remove
// Run backfilling at a reduced priority
#define BACKFILL_CACHE_PRIORITY 10

void btree_slice_t::init_real_superblock(real_superblock_t *superblock,
                                         rockshard rocksh,
                                         const std::vector<char> &metainfo_key,
                                         const binary_blob_t &metainfo_value) {
    superblock->write_acq_signal()->wait_lazily_ordered();
    set_superblock_metainfo(superblock, rocksh, metainfo_key, metainfo_value);

    sindex_block_lock sindex_block(superblock->get(), access_t::write);
    initialize_secondary_indexes(rocksh, &sindex_block);
}

void btree_slice_t::init_sindex_superblock(sindex_superblock_t *superblock) {
    superblock->write_acq_signal()->wait_lazily_ordered();
    // Nothing to do.
    // TODO: Just get rid of the locking logic, this function entirely?
}

btree_slice_t::btree_slice_t(cache_t *c, perfmon_collection_t *parent,
                             const std::string &identifier,
                             index_type_t index_type)
    : stats(parent,
            (index_type == index_type_t::SECONDARY ? "index-" : "") + identifier),
      cache_(c),
      backfill_account_(cache()->create_cache_account(BACKFILL_CACHE_PRIORITY)) { }

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
        real_superblock_t *superblock,
        std::vector<std::pair<std::vector<char>, std::vector<char> > > *kv_pairs_out) {
    superblock->read_acq_signal()->wait_lazily_ordered();

    std::string meta_prefix = rockstore::table_metadata_prefix(rocksh.table_id, rocksh.shard_no);
    std::string version
        = rocksh.rocks->read(meta_prefix + rockstore::TABLE_METADATA_VERSION_KEY());
    std::string metainfo
        = rocksh.rocks->read(meta_prefix + rockstore::TABLE_METADATA_METAINFO_KEY());

    // TODO: Do we even need this field?
    if (version != rockstore::VERSION()) {
        crash("Unrecognized metainfo version found.");
    }

    for (superblock_metainfo_iterator_t kv_iter(metainfo.data(), metainfo.data() + metainfo.size()); !kv_iter.is_end(); ++kv_iter) {
        superblock_metainfo_iterator_t::key_t key = kv_iter.key();
        superblock_metainfo_iterator_t::value_t value = kv_iter.value();
        kv_pairs_out->push_back(std::make_pair(std::vector<char>(key.second, key.second + key.first), std::vector<char>(value.second, value.second + value.first)));
    }
}

void set_superblock_metainfo(real_superblock_t *superblock,
                             rockshard rocksh,
                             const std::vector<char> &key,
                             const binary_blob_t &value) {
    std::vector<std::vector<char> > keys = {key};
    std::vector<binary_blob_t> values = {value};
    set_superblock_metainfo(superblock, rocksh, keys, values);
}

void set_superblock_metainfo(real_superblock_t *superblock,
                             rockshard rocksh,
                             const std::vector<std::vector<char> > &keys,
                             const std::vector<binary_blob_t> &values) {
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
        rassert(value_it->size() < UINT32_MAX);

        u.y = key_it->size();
        metainfo.insert(metainfo.end(), u.x, u.x + sizeof(uint32_t));
        metainfo.insert(metainfo.end(), key_it->begin(), key_it->end());

        u.y = value_it->size();
        metainfo.insert(metainfo.end(), u.x, u.x + sizeof(uint32_t));
        metainfo.insert(
            metainfo.end(),
            static_cast<const uint8_t *>(value_it->data()),
            static_cast<const uint8_t *>(value_it->data()) + value_it->size());
    }

    // TODO: buffer_group_copy_data -- does anybody use it?

    // Rocksdb metadata.
    rocksdb::WriteBatch batch;
    std::string meta_prefix = rockstore::table_metadata_prefix(rocksh.table_id, rocksh.shard_no);
    // TODO: Don't update version if it's already properly set.  (Performance.)
    // TODO: Just remove the metadata version key...?
    rocksdb::Status status = batch.Put(
        meta_prefix + rockstore::TABLE_METADATA_VERSION_KEY(),
        rockstore::VERSION());
    guarantee(status.ok());
    status = batch.Put(
        meta_prefix + rockstore::TABLE_METADATA_METAINFO_KEY(),
        rocksdb::Slice(metainfo.data(), metainfo.size()));
    guarantee(status.ok());
    rocksh->write_batch(std::move(batch), rockstore::write_options::TODO());
}

void get_btree_superblock(
        txn_t *txn,
        access_t access,
        scoped_ptr_t<real_superblock_t> *got_superblock_out) {
    real_superblock_lock tmp_buf(txn, access);
    scoped_ptr_t<real_superblock_t> tmp_sb(new real_superblock_t(std::move(tmp_buf)));
    *got_superblock_out = std::move(tmp_sb);
}

/* Variant for writes that go through a superblock write semaphore */
void get_btree_superblock(
        txn_t *txn,
        UNUSED write_access_t access,
        new_semaphore_in_line_t &&write_sem_acq,
        scoped_ptr_t<real_superblock_t> *got_superblock_out) {
    real_superblock_lock tmp_buf(txn, access_t::write);
    scoped_ptr_t<real_superblock_t> tmp_sb(
        new real_superblock_t(std::move(tmp_buf), std::move(write_sem_acq)));
    *got_superblock_out = std::move(tmp_sb);
}

void get_btree_superblock_and_txn_for_writing(
        cache_conn_t *cache_conn,
        new_semaphore_t *superblock_write_semaphore,
        UNUSED write_access_t superblock_access,
        int expected_change_count,
        write_durability_t durability,
        scoped_ptr_t<real_superblock_t> *got_superblock_out,
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
        scoped_ptr_t<real_superblock_t> *got_superblock_out,
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
        scoped_ptr_t<real_superblock_t> *got_superblock_out,
        scoped_ptr_t<txn_t> *txn_out) {
    txn_t *txn = new txn_t(cache_conn, read_access_t::read);
    txn_out->init(txn);

    get_btree_superblock(txn, access_t::read, got_superblock_out);
}
