// Copyright 2010-2015 RethinkDB, all rights reserved.
#ifndef BTREE_REQL_SPECIFIC_HPP_
#define BTREE_REQL_SPECIFIC_HPP_

#include "btree/operations.hpp"
#include "btree/stats.hpp"
#include "buffer_cache/alt.hpp"
#include "containers/uuid.hpp"
#include "concurrency/new_semaphore.hpp"
#include "concurrency/promise.hpp"

class cache_t;
class rockshard;
class version_t;

class superblock_passback_guard {
public:
    superblock_passback_guard(real_superblock_lock *_superblock, promise_t<real_superblock_lock *> *_pass_back)
        : superblock(_superblock), pass_back_superblock(_pass_back) {
        rassert(superblock != nullptr);
        rassert(pass_back_superblock != nullptr);
    }
    DISABLE_COPYING(superblock_passback_guard);
    ~superblock_passback_guard() {
        pass_back_superblock->pulse(superblock);
    }
    real_superblock_lock *superblock;
    promise_t<real_superblock_lock *> *pass_back_superblock;
};

enum class index_type_t {
    PRIMARY,
    SECONDARY
};

/* btree_slice_t is a thin wrapper around cache_t that handles initializing the buffer
cache for the purpose of storing a B-tree. It is specific to ReQL primary and secondary
index B-trees. */

class btree_slice_t : public home_thread_mixin_debug_only_t {
public:
    // Initializes a superblock (presumably, constructed with
    // alt_create_t::create) for use with btrees, setting the initial value of
    // the metainfo (with a single key/value pair). Not for use with sindex
    // superblocks.
    static void init_real_superblock(
            real_superblock_lock *superblock,
            rockshard rocksh,
            const std::vector<char> &metainfo_key,
            const version_t &metainfo_value);

    btree_slice_t(cache_t *cache,
                  perfmon_collection_t *parent,
                  const std::string &identifier,
                  index_type_t index_type);

    ~btree_slice_t();

    cache_t *cache() { return cache_; }

    btree_stats_t stats;

private:
    cache_t *cache_;

    DISABLE_COPYING(btree_slice_t);
};

/* This iterator encapsulates most of the metainfo data layout. Unfortunately,
 * functions set_superblock_metainfo and delete_superblock_metainfo also know a
 * lot about the data layout, so if it's changed, these functions must be
 * changed as well.
 *
 * Data layout is dead simple right now, it's an array of the following
 * (unaligned, unpadded) contents:
 *
 *   sz_t key_size;
 *   char key[key_size];
 *   sz_t value_size;
 *   char value[value_size];
 */
struct superblock_metainfo_iterator_t {
    typedef uint32_t sz_t;  // be careful: the values of this type get casted to int64_t in checks, so it must fit
    typedef std::pair<sz_t, const char *> key_t;
    typedef std::pair<sz_t, const char *> value_t;

    superblock_metainfo_iterator_t(const char *metainfo, const char *metainfo_end) : end(metainfo_end) { advance(metainfo); }

    bool is_end() { return pos == end; }

    void operator++();

    std::pair<key_t, value_t> operator*() {
        return std::make_pair(key(), value());
    }
    key_t key() { return std::make_pair(key_size, key_ptr); }
    value_t value() { return std::make_pair(value_size, value_ptr); }

    const char *record_ptr() { return pos; }
    const char *next_record_ptr() { return next_pos; }
    const char *end_ptr() { return end; }
    const sz_t *value_size_ptr() { return reinterpret_cast<const sz_t*>(value_ptr) - 1; }
private:
    void advance(const char *p);

    const char *pos;
    const char *next_pos;
    const char *end;
    sz_t key_size;
    const char *key_ptr;
    sz_t value_size;
    const char *value_ptr;
};


// Metainfo functions
void get_superblock_metainfo(
    rockshard rocksh,
    real_superblock_lock *superblock,
    std::vector< std::pair<std::vector<char>, std::vector<char> > > *kv_pairs_out);

void set_superblock_metainfo(real_superblock_lock *superblock,
                             rockshard rocksh,
                             const std::vector<char> &key,
                             const version_t &value);

void set_superblock_metainfo(real_superblock_lock *superblock,
                             rockshard rocksh,
                             const std::vector<std::vector<char> > &keys,
                             const std::vector<version_t> &values);

// Convenience functions for accessing the superblock
void get_btree_superblock(
        txn_t *txn,
        access_t access,
        scoped_ptr_t<real_superblock_lock> *got_superblock_out);

/* Variant for writes that go through a superblock write semaphore */
void get_btree_superblock(
        txn_t *txn,
        write_access_t access,
        new_semaphore_in_line_t &&write_sem_acq,
        scoped_ptr_t<real_superblock_lock> *got_superblock_out);

void get_btree_superblock_and_txn_for_writing(
        cache_conn_t *cache_conn,
        new_semaphore_t *superblock_write_semaphore,
        write_access_t superblock_access,
        int expected_change_count,
        write_durability_t durability,
        scoped_ptr_t<real_superblock_lock> *got_superblock_out,
        scoped_ptr_t<txn_t> *txn_out);

// TODO: The fact that we have this might mean our backfilling logic is wrong
// with respect to snapshots.  Take a look.
void get_btree_superblock_and_txn_for_backfilling(
        cache_conn_t *cache_conn,
        cache_account_t *backfill_account,
        scoped_ptr_t<real_superblock_lock> *got_superblock_out,
        scoped_ptr_t<txn_t> *txn_out);

void get_btree_superblock_and_txn_for_reading(
        cache_conn_t *cache_conn,
        scoped_ptr_t<real_superblock_lock> *got_superblock_out,
        scoped_ptr_t<txn_t> *txn_out);

#endif /* BTREE_REQL_SPECIFIC_HPP_ */

