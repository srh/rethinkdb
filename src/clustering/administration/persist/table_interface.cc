// Copyright 2010-2015 RethinkDB, all rights reserved.
#include "clustering/administration/persist/table_interface.hpp"

#include <algorithm>
#include <array>

#include "rocksdb/write_batch.h"

#include "arch/io/disk.hpp"
#include "clustering/administration/persist/branch_history_manager.hpp"
#include "clustering/administration/persist/file_keys.hpp"
#include "clustering/administration/persist/raft_storage_interface.hpp"
#include "clustering/administration/perfmon_collection_repo.hpp"
#include "clustering/table_contract/store_ptr.hpp"
#include "logger.hpp"
#include "rdb_protocol/store.hpp"

class real_store_ptr_t :
    public store_ptr_t {
public:
    real_store_ptr_t(
            rockstore::store *rocks,
            const namespace_id_t &table_id,
            scoped_ptr_t<real_branch_history_manager_t> &&bhm,
            const base_path_t &base_path,
            io_backender_t *io_backender,
            rdb_context_t *rdb_context,
            perfmon_collection_t *perfmon_collection_serializers,
            scoped_ptr_t<thread_allocation_t> &&store_thread,
            std::map<
                namespace_id_t, std::pair<real_store_ptr_t *, auto_drainer_t::lock_t>
            > *real_multistores) :
        branch_history_manager(std::move(bhm)),
        store_thread_allocation(std::move(store_thread)),
        map_insertion_sentry(
            real_multistores, table_id, std::make_pair(this, drainer.lock()))
    {
        // TODO: If the server gets killed when starting up, we can
        // get a database in an invalid startup state.

        std::string existence_key = rockstore::table_existence_key(table_id);
        bool exists = rocks->try_read(existence_key).second;
        bool create = !exists;

        // We might want to wipe out the entire key range here, for hygiene --
        // or wipe out just the metainfo and assert there is nothing else in the
        // actual table shards.

        {
            // TODO: Exceptions? If exceptions are being thrown in here, nothing is
            // handling them.

            on_thread_t thread_switcher_2(store_thread_allocation->get_thread());

            store.init(new store_t(
                rocks,
                "shard",
                create,
                perfmon_collection_serializers,
                rdb_context,
                io_backender,
                base_path,
                table_id,
                update_sindexes_t::UPDATE));
        };

        // We create existence key after we've actually created and initialized
        // the stores (and before any data writes could happen).
        if (create) {
            // TODO: Handle this transactionally (so we aren't stuck in an
            // invalid state after putting the existence key but not creating
            // the stores.)
            rocks->deprecated_put(existence_key, "1", rockstore::write_options(true));
        }
    }

    ~real_store_ptr_t() {
        store_thread_allocation.reset();
        map_insertion_sentry.reset();
        drainer.drain();

        if (store.has()) {
            on_thread_t thread_switcher(store->home_thread());
            store.reset();
        }
    }

    branch_history_manager_t *get_branch_history_manager() {
        return branch_history_manager.get();
    }

    store_view_t *get_store() {
        return store.get();
    }

    store_t *get_underlying_store() {
        return store.get();
    }

private:
    scoped_ptr_t<real_branch_history_manager_t> branch_history_manager;
    scoped_ptr_t<store_t> store;

    scoped_ptr_t<thread_allocation_t> store_thread_allocation;

    auto_drainer_t drainer;
    map_insertion_sentry_t<
        namespace_id_t, std::pair<real_store_ptr_t *, auto_drainer_t::lock_t>
    > map_insertion_sentry;

    DISABLE_COPYING(real_store_ptr_t);
};

void real_table_persistence_interface_t::read_all_metadata(
        const std::function<void(
            const namespace_id_t &table_id,
            const table_active_persistent_state_t &state,
            raft_storage_interface_t<table_raft_state_t> *raft_storage,
            metadata_file_t::read_txn_t *metadata_read_txn)> &active_cb,
        const std::function<void(
            const namespace_id_t &table_id,
            const table_inactive_persistent_state_t &state,
            metadata_file_t::read_txn_t *metadata_read_txn)> &inactive_cb,
        signal_t *interruptor) {
    metadata_file_t::read_txn_t read_txn(metadata_file, interruptor);

    std::map<namespace_id_t, table_active_persistent_state_t> active_tables;
    read_txn.read_many<table_active_persistent_state_t>(
        mdprefix_table_active(),
        [&](const std::string &uuid_str, const table_active_persistent_state_t &state) {
            active_tables[str_to_uuid(uuid_str)] = state;
        },
        interruptor);
    storage_interfaces.clear();
    for (const auto &pair : active_tables) {
        storage_interfaces[pair.first].init(new table_raft_storage_interface_t(
            metadata_file, &read_txn, pair.first, interruptor));
        active_cb(
            pair.first, pair.second, storage_interfaces[pair.first].get(), &read_txn);
    }

    read_txn.read_many<table_inactive_persistent_state_t>(
        mdprefix_table_inactive(),
        [&](const std::string &uuid_str, const table_inactive_persistent_state_t &s) {
            inactive_cb(str_to_uuid(uuid_str), s, &read_txn);
        },
        interruptor);
}

void real_table_persistence_interface_t::write_metadata_active(
        const namespace_id_t &table_id,
        const table_active_persistent_state_t &state,
        const raft_persistent_state_t<table_raft_state_t> &raft_state,
        raft_storage_interface_t<table_raft_state_t> **raft_storage_out) {
    cond_t non_interruptor;
    storage_interfaces.erase(table_id);
    metadata_file_t::write_txn_t write_txn(metadata_file, &non_interruptor);
    write_txn.erase(
        mdprefix_table_inactive().suffix(uuid_to_str(table_id)),
        &non_interruptor);
    table_raft_storage_interface_t::erase(&write_txn, table_id);
    write_txn.write(
        mdprefix_table_active().suffix(uuid_to_str(table_id)),
        state,
        &non_interruptor);
    storage_interfaces[table_id].init(new table_raft_storage_interface_t(
        metadata_file, &write_txn, table_id, raft_state));
    *raft_storage_out = storage_interfaces[table_id].get();
    write_txn.commit();
}

void real_table_persistence_interface_t::write_metadata_inactive(
        const namespace_id_t &table_id,
        const table_inactive_persistent_state_t &state) {
    cond_t non_interruptor;
    storage_interfaces.erase(table_id);
    metadata_file_t::write_txn_t write_txn(metadata_file, &non_interruptor);
    write_txn.erase(
        mdprefix_table_active().suffix(uuid_to_str(table_id)),
        &non_interruptor);
    write_txn.write(
        mdprefix_table_inactive().suffix(uuid_to_str(table_id)),
        state,
        &non_interruptor);
    table_raft_storage_interface_t::erase(&write_txn, table_id);
    real_branch_history_manager_t::erase(&write_txn, table_id);
    write_txn.commit();
}

void real_table_persistence_interface_t::delete_metadata(
        const namespace_id_t &table_id) {
    cond_t non_interruptor;
    storage_interfaces.erase(table_id);
    metadata_file_t::write_txn_t write_txn(metadata_file, &non_interruptor);
    write_txn.erase(
        mdprefix_table_active().suffix(uuid_to_str(table_id)),
        &non_interruptor);
    write_txn.erase(
        mdprefix_table_inactive().suffix(uuid_to_str(table_id)),
        &non_interruptor);
    table_raft_storage_interface_t::erase(&write_txn, table_id);
    real_branch_history_manager_t::erase(&write_txn, table_id);
    write_txn.commit();
}

void real_table_persistence_interface_t::load_multistore(
        const namespace_id_t &table_id,
        metadata_file_t::read_txn_t *metadata_read_txn,
        scoped_ptr_t<store_ptr_t> *multistore_ptr_out,
        signal_t *interruptor,
        perfmon_collection_t *perfmon_collection_serializers) {
    scoped_ptr_t<real_branch_history_manager_t> bhm(
        new real_branch_history_manager_t(
            table_id, metadata_file, metadata_read_txn, interruptor));

    auto store_thread = make_scoped<thread_allocation_t>(&thread_allocator);

    multistore_ptr_out->init(new real_store_ptr_t(
        io_backender->rocks(),
        table_id,
        std::move(bhm),
        base_path,
        io_backender,
        rdb_context,
        perfmon_collection_serializers,
        std::move(store_thread),
        &real_multistores));
}

void real_table_persistence_interface_t::create_multistore(
        const namespace_id_t &table_id,
        scoped_ptr_t<store_ptr_t> *multistore_ptr_out,
        signal_t *interruptor,
        perfmon_collection_t *perfmon_collection_serializers) {
    metadata_file_t::read_txn_t read_txn(metadata_file, interruptor);
    load_multistore(
        table_id, &read_txn, multistore_ptr_out, interruptor,
        perfmon_collection_serializers);
}

void real_table_persistence_interface_t::destroy_multistore(
        const namespace_id_t &table_id,
        scoped_ptr_t<store_ptr_t> *multistore_ptr_in) {
    guarantee(multistore_ptr_in->has());
    multistore_ptr_in->reset();

    std::string prefix = rockstore::table_prefix(table_id);
    std::string end_prefix = rockstore::prefix_end(prefix);

    rocksdb::WriteBatch batch;
    batch.DeleteRange(prefix, end_prefix);
    io_backender->rocks()->write_batch(&batch, rockstore::write_options(true));
}

bool real_table_persistence_interface_t::is_gc_active() const {
    // TODO: Is there a rocksdb version of this?

    return false;
}
