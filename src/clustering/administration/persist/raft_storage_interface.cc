// File has been changed by Sam Hughes (to add soft durability flush interval)
// Copyright 2010-2015 RethinkDB, all rights reserved.
#include "clustering/administration/persist/raft_storage_interface.hpp"

#include "clustering/administration/persist/file_keys.hpp"

RDB_IMPL_SERIALIZABLE_3_SINCE_v2_1(table_raft_stored_header_t,
    current_term, voted_for, commit_index);
RDB_IMPL_SERIALIZABLE_4_SINCE_v2_1(table_raft_stored_snapshot_t,
    snapshot_state, snapshot_config, log_prev_index, log_prev_term);
RDB_IMPL_SERIALIZABLE_3_SINCE_v2_1(table_raft_versioned_user_value_t,
    log_prev_term, log_prev_index, user_value);

raft_log_index_t str_to_log_index(const std::string &str) {
    guarantee(str.size() == 16);
    raft_log_index_t index = 0;
    for (size_t i = 0; i < 16; ++i) {
        int val;
        if (str[i] >= '0' && str[i] <= '9') {
            val = str[i] - '0';
        } else if (str[i] >= 'a' && str[i] <= 'f') {
            val = 10 + (str[i] - 'a');
        } else {
            crash("bad character in str_to_log_index()");
        }
        index += val << ((15 - i) * 4);
    }
    return index;
}

std::string log_index_to_str(raft_log_index_t log_index) {
    std::string str;
    str.reserve(16);
    for (size_t i = 0; i < 16; ++i) {
        int val = (log_index >> ((15 - i) * 4)) & 0x0f;
        rassert(val >= 0 && val < 16);
        str.push_back("0123456789abcdef"[val]);
    }
    return str;
}

user_value_t *snapshot_user_value(table_raft_stored_snapshot_t *snapshot) {
    return &snapshot->snapshot_state.config.config.user_value;
}

void update_user_value(table_raft_stored_snapshot_t *snapshot,
                       user_value_t &&user_value) {
    *snapshot_user_value(snapshot) = std::move(user_value);
}

bool uses_extension(const raft_log_entry_t<table_raft_state_t> *entry,
                    const user_value_t **uv_ptr_out) {
    if (!entry->change) {
        return false;
    }
    auto *set_config = boost::get<table_raft_state_t::change_t::set_table_config_t>(&entry->change->v);
    if (set_config == nullptr) {
        return false;
    }
    *uv_ptr_out = &set_config->new_config.config.user_value;
    return true;
}

bool uses_extension(raft_log_entry_t<table_raft_state_t> *entry,
                    user_value_t **uv_ptr_out) {
    if (!entry->change) {
        return false;
    }
    auto *set_config = boost::get<table_raft_state_t::change_t::set_table_config_t>(&entry->change->v);
    if (set_config == nullptr) {
        return false;
    }
    *uv_ptr_out = &set_config->new_config.config.user_value;
    return true;
}

table_raft_storage_interface_t::table_raft_storage_interface_t(
        metadata_file_t *_file,
        metadata_file_t::read_txn_t *txn,
        const namespace_id_t &_table_id,
        signal_t *interruptor) :
        file(_file), table_id(_table_id) {
    std::string table_id_string = uuid_to_str(table_id);
    table_raft_stored_header_t header = txn->read(
        mdprefix_table_raft_header().suffix(table_id_string), interruptor);
    state.current_term = header.current_term;
    state.voted_for = header.voted_for;
    state.commit_index = header.commit_index;
    table_raft_stored_snapshot_t snapshot = txn->read(
        mdprefix_table_raft_snapshot().suffix(table_id_string), interruptor);
    table_raft_versioned_user_value_t versioned_uv;
    if (txn->read_maybe<table_raft_versioned_user_value_t, cluster_version_t::v2_4_ext>(
        mdprefix_table_raft_snapshot_extension().suffix(table_id_string),
        &versioned_uv,
        interruptor)) {
        if (versioned_uv.log_prev_index == snapshot.log_prev_index
            && versioned_uv.log_prev_term == snapshot.log_prev_term) {
            update_user_value(&snapshot, std::move(versioned_uv.user_value));
        }
    }
    state.snapshot_state = std::move(snapshot.snapshot_state);
    state.snapshot_config = std::move(snapshot.snapshot_config);
    state.log.prev_index = snapshot.log_prev_index;
    state.log.prev_term = snapshot.log_prev_term;
    txn->read_many<raft_log_entry_t<table_raft_state_t> >(
        mdprefix_table_raft_log().suffix(table_id_string + "/"),
        [&](std::string &&index_str,
            raft_log_entry_t<table_raft_state_t> &&entry) {
            guarantee(str_to_log_index(index_str) == state.log.get_latest_index() + 1,
                "%" PRIu64 " ('%s') == %" PRIu64,
                str_to_log_index(index_str),
                index_str.c_str(),
                state.log.get_latest_index() + 1);

            user_value_t *uv;
            if (uses_extension(&entry, &uv)) {
                if (!txn->read_maybe<user_value_t, cluster_version_t::v2_4_ext>(
                        mdprefix_table_raft_log_extension().suffix(table_id_string + "/" + index_str),
                        uv,
                        interruptor)) {
                    *uv = default_user_value();
                }
            }

            state.log.append(entry);
        },
        interruptor);
}

// Moves values out of the snapshot (you have to move them back in with deconstruct...)
table_raft_versioned_user_value_t construct_uv_from_snapshot(
        table_raft_stored_snapshot_t *snapshot) {
    table_raft_versioned_user_value_t ret;
    ret.log_prev_index = snapshot->log_prev_index;
    ret.log_prev_term = snapshot->log_prev_term;
    ret.user_value = std::move(*snapshot_user_value(snapshot));
    return ret;
}

void deconstruct_uv_into_snapshot(
        table_raft_stored_snapshot_t *snapshot,
        table_raft_versioned_user_value_t &&uv) {
    *snapshot_user_value(snapshot) = std::move(uv.user_value);
}


table_raft_storage_interface_t::table_raft_storage_interface_t(
        metadata_file_t *_file,
        metadata_file_t::write_txn_t *txn,
        const namespace_id_t &_table_id,
        const raft_persistent_state_t<table_raft_state_t> &_state) :
        file(_file), table_id(_table_id), state(_state) {
    cond_t non_interruptor;
    std::string table_id_string = uuid_to_str(table_id);
    txn->write(
        mdprefix_table_raft_header().suffix(table_id_string),
        table_raft_stored_header_t::from_state(state),
        &non_interruptor);

    /* To avoid expensive copies of `state`, we move `state` into the snapshot and then
    back out after we're done */
    table_raft_stored_snapshot_t snapshot;
    snapshot.snapshot_state = std::move(state.snapshot_state);
    snapshot.snapshot_config = std::move(state.snapshot_config);
    snapshot.log_prev_index = state.log.prev_index;
    snapshot.log_prev_term = state.log.prev_term;
    txn->write(
        mdprefix_table_raft_snapshot().suffix(table_id_string),
        snapshot,
        &non_interruptor);
    // We also move the user_value into versioned_uv and then back out after we're done.
    table_raft_versioned_user_value_t versioned_uv
        = construct_uv_from_snapshot(&snapshot);
    txn->write<table_raft_versioned_user_value_t, cluster_version_t::v2_4_ext>(
        mdprefix_table_raft_snapshot_extension().suffix(table_id_string),
        versioned_uv,
        &non_interruptor);
    deconstruct_uv_into_snapshot(&snapshot, std::move(versioned_uv));
    state.snapshot_state = std::move(snapshot.snapshot_state);
    state.snapshot_config = std::move(snapshot.snapshot_config);

    for (raft_log_index_t i = state.log.prev_index + 1;
            i <= state.log.get_latest_index(); ++i) {
        const raft_log_entry_t<table_raft_state_t> &entry = state.log.get_entry_ref(i);
        txn->write(
            mdprefix_table_raft_log().suffix(
                uuid_to_str(table_id) + "/" + log_index_to_str(i)),
            entry,
            &non_interruptor);

        const user_value_t *uv;
        if (uses_extension(&entry, &uv)) {
            txn->write<user_value_t, cluster_version_t::v2_4_ext>(
                mdprefix_table_raft_log_extension().suffix(
                    uuid_to_str(table_id) + "/" + log_index_to_str(i)),
                *uv,
                &non_interruptor);
        }
    }
}

void table_raft_storage_interface_t::erase(
        metadata_file_t::write_txn_t *txn,
        const namespace_id_t &table_id) {
    std::string table_id_string = uuid_to_str(table_id);
    cond_t non_interruptor;
    txn->erase(
        mdprefix_table_raft_header().suffix(table_id_string),
        &non_interruptor);
    txn->erase(
        mdprefix_table_raft_snapshot().suffix(table_id_string),
        &non_interruptor);
    txn->erase(
        mdprefix_table_raft_snapshot_extension().suffix(table_id_string),
        &non_interruptor);
    std::vector<std::string> log_keys;
    txn->read_many<raft_log_entry_t<table_raft_state_t> >(
        mdprefix_table_raft_log().suffix(table_id_string + "/"),
        [&](std::string &&index_str, raft_log_entry_t<table_raft_state_t> &&) {
            log_keys.push_back(std::move(index_str));
        },
        &non_interruptor);
    for (const std::string &key : log_keys) {
        std::string key_suffix = table_id_string + "/" + key;
        txn->erase(
            mdprefix_table_raft_log().suffix(key_suffix),
            &non_interruptor);
        txn->erase(
            mdprefix_table_raft_log_extension().suffix(key_suffix),
            &non_interruptor);
    }
}

const raft_persistent_state_t<table_raft_state_t> *
table_raft_storage_interface_t::get() {
    return &state;
}

void table_raft_storage_interface_t::write_current_term_and_voted_for(
        raft_term_t current_term,
        raft_member_id_t voted_for) {
    cond_t non_interruptor;
    metadata_file_t::write_txn_t txn(file, &non_interruptor);
    state.current_term = current_term;
    state.voted_for = voted_for;
    txn.write(
        mdprefix_table_raft_header().suffix(uuid_to_str(table_id)),
        table_raft_stored_header_t::from_state(state),
        &non_interruptor);
    txn.commit();
}

void table_raft_storage_interface_t::write_commit_index(
        raft_log_index_t commit_index) {
    cond_t non_interruptor;
    metadata_file_t::write_txn_t txn(file, &non_interruptor);
    state.commit_index = commit_index;
    txn.write(
        mdprefix_table_raft_header().suffix(uuid_to_str(table_id)),
        table_raft_stored_header_t::from_state(state),
        &non_interruptor);
    txn.commit();
}

void table_raft_storage_interface_t::write_log_replace_tail(
        const raft_log_t<table_raft_state_t> &source,
        raft_log_index_t first_replaced) {
    cond_t non_interruptor;
    metadata_file_t::write_txn_t txn(file, &non_interruptor);
    guarantee(first_replaced > state.log.prev_index);
    guarantee(first_replaced <= state.log.get_latest_index() + 1);
    for (raft_log_index_t i = first_replaced;
            i <= std::max(state.log.get_latest_index(), source.get_latest_index()); ++i) {
        std::string key_suffix = uuid_to_str(table_id) + "/" + log_index_to_str(i);
        metadata_file_t::key_t<raft_log_entry_t<table_raft_state_t> > key =
            mdprefix_table_raft_log().suffix(key_suffix);
        if (i <= source.get_latest_index()) {
            const raft_log_entry_t<table_raft_state_t> &entry = source.get_entry_ref(i);
            txn.write(key, entry, &non_interruptor);
            const user_value_t *uv;
            if (uses_extension(&entry, &uv)) {
                txn.write<user_value_t, cluster_version_t::v2_4_ext>(
                    mdprefix_table_raft_log_extension().suffix(key_suffix),
                    *uv,
                    &non_interruptor);
            }
        } else {
            txn.erase(key, &non_interruptor);
            txn.erase(
                mdprefix_table_raft_log_extension().suffix(key_suffix),
                &non_interruptor);
        }
    }
    if (first_replaced != state.log.get_latest_index() + 1) {
        state.log.delete_entries_from(first_replaced);
    }
    for (raft_log_index_t i = first_replaced; i <= source.get_latest_index(); ++i) {
        state.log.append(source.get_entry_ref(i));
    }
    txn.commit();
}

void table_raft_storage_interface_t::write_log_append_one(
        const raft_log_entry_t<table_raft_state_t> &entry) {
    cond_t non_interruptor;
    metadata_file_t::write_txn_t txn(file, &non_interruptor);
    raft_log_index_t index = state.log.get_latest_index() + 1;
    std::string key_suffix = uuid_to_str(table_id) + "/" + log_index_to_str(index);
    txn.write(
        mdprefix_table_raft_log().suffix(key_suffix),
        entry,
        &non_interruptor);
    const user_value_t *uv;
    if (uses_extension(&entry, &uv)) {
        txn.write<user_value_t, cluster_version_t::v2_4_ext>(
            mdprefix_table_raft_log_extension().suffix(key_suffix),
            *uv,
            &non_interruptor);
    }
    state.log.append(entry);
    txn.commit();
}

void table_raft_storage_interface_t::write_snapshot(
        const table_raft_state_t &snapshot_state,
        const raft_complex_config_t &snapshot_config,
        bool clear_log,
        raft_log_index_t log_prev_index,
        raft_term_t log_prev_term,
        raft_log_index_t commit_index) {
    std::string table_id_string = uuid_to_str(table_id);
    cond_t non_interruptor;
    metadata_file_t::write_txn_t txn(file, &non_interruptor);
    state.commit_index = commit_index;
    txn.write(
        mdprefix_table_raft_header().suffix(table_id_string),
        table_raft_stored_header_t::from_state(state),
        &non_interruptor);
    table_raft_stored_snapshot_t snapshot;
    snapshot.snapshot_state = snapshot_state;
    snapshot.snapshot_config = snapshot_config;
    snapshot.log_prev_index = log_prev_index;
    snapshot.log_prev_term = log_prev_term;
    txn.write(
        mdprefix_table_raft_snapshot().suffix(table_id_string),
        snapshot,
        &non_interruptor);
    table_raft_versioned_user_value_t versioned_uv = construct_uv_from_snapshot(&snapshot);
    txn.write<table_raft_versioned_user_value_t, cluster_version_t::v2_4_ext>(
        mdprefix_table_raft_snapshot_extension().suffix(table_id_string),
        versioned_uv,
        &non_interruptor);
    deconstruct_uv_into_snapshot(&snapshot, std::move(versioned_uv));


    for (raft_log_index_t i = state.log.prev_index + 1;
            i <= (clear_log ? state.log.get_latest_index() : log_prev_index); ++i) {
        std::string key_suffix = uuid_to_str(table_id) + "/" + log_index_to_str(i);
        txn.erase(
            mdprefix_table_raft_log().suffix(key_suffix),
            &non_interruptor);
        txn.erase(
            mdprefix_table_raft_log_extension().suffix(key_suffix),
            &non_interruptor);
    }
    state.snapshot_state = std::move(snapshot.snapshot_state);
    state.snapshot_config = std::move(snapshot.snapshot_config);
    if (clear_log) {
        state.log.entries.clear();
        state.log.prev_index = log_prev_index;
        state.log.prev_term = log_prev_term;
    } else {
        state.log.delete_entries_to(log_prev_index, log_prev_term);
    }
    txn.commit();
}

