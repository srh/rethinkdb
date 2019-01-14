// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "serializer/translator.hpp"

#include "concurrency/new_mutex.hpp"
#include "concurrency/pmap.hpp"
#include "debug.hpp"
#include "serializer/buf_ptr.hpp"
#include "serializer/types.hpp"
#include "utils.hpp"

/* translator_serializer_t */

block_id_t translator_serializer_t::translate_block_id(block_id_t id, int mod_count, int mod_id, config_block_id_t cfgid) {
    if (is_aux_block_id(id)) {
        return FIRST_AUX_BLOCK_ID + translate_block_id(make_aux_block_id_relative(id), mod_count, mod_id, cfgid);
    } else {
        return id * mod_count + mod_id + cfgid.subsequent_ser_id();
    }
}

int translator_serializer_t::untranslate_block_id_to_mod_id(block_id_t inner_id, int mod_count, config_block_id_t cfgid) {
    if (is_aux_block_id(inner_id)) {
        return untranslate_block_id_to_mod_id(make_aux_block_id_relative(inner_id), mod_count, cfgid);
    } else {
        // We know that inner_id == id * mod_count + mod_id + min.
        // Thus inner_id - min == id * mod_count + mod_id.
        // It follows that inner_id - min === mod_id (modulo mod_count).
        // So (inner_id - min) % mod_count == mod_id (since 0 <= mod_id < mod_count).
        // (And inner_id - min >= 0, so '%' works as expected.)
        return (inner_id - cfgid.subsequent_ser_id()) % mod_count;
    }
}

block_id_t translator_serializer_t::untranslate_block_id_to_id(block_id_t inner_id, int mod_count, int mod_id, config_block_id_t cfgid) {
    if (is_aux_block_id(inner_id)) {
        return FIRST_AUX_BLOCK_ID + untranslate_block_id_to_id(
            make_aux_block_id_relative(inner_id), mod_count, mod_id, cfgid);
    } else {
        // (simply dividing by mod_count should be sufficient, but this is cleaner)
        return (inner_id - cfgid.subsequent_ser_id() - mod_id) / mod_count;
    }
}

block_id_t translator_serializer_t::translate_block_id(block_id_t id) const {
    rassert(id != NULL_BLOCK_ID);
    return translate_block_id(id, mod_count, mod_id, cfgid);
}

translator_serializer_t::translator_serializer_t(serializer_t *_inner, int _mod_count, int _mod_id, config_block_id_t _cfgid)
    : inner(_inner), mod_count(_mod_count), mod_id(_mod_id), cfgid(_cfgid), read_ahead_callback(nullptr) {
    rassert(mod_count > 0);
    rassert(mod_id >= 0);
    rassert(mod_id < mod_count);
}

file_account_t *translator_serializer_t::make_io_account(int priority, int outstanding_requests_limit) {
    return inner->make_io_account(priority, outstanding_requests_limit);
}

void translator_serializer_t::index_write(
        new_mutex_in_line_t *mutex_acq,
        const std::function<void()> &on_writes_reflected,
        const std::vector<index_write_op_t> &write_ops) {
    std::vector<index_write_op_t> translated_ops(write_ops);
    for (auto it = translated_ops.begin(); it < translated_ops.end(); ++it) {
        it->block_id = translate_block_id(it->block_id);
    }
    inner->index_write(mutex_acq, on_writes_reflected, translated_ops);
}

std::vector<counted_t<standard_block_token_t> >
translator_serializer_t::block_writes(const std::vector<buf_write_info_t> &write_infos,
                                      file_account_t *io_account, iocallback_t *cb) {
    std::vector<buf_write_info_t> tmp;
    tmp.reserve(write_infos.size());
    for (auto it = write_infos.begin(); it != write_infos.end(); ++it) {
        guarantee(it->block_id != NULL_BLOCK_ID);
        tmp.push_back(buf_write_info_t(it->buf, it->block_size,
                                       translate_block_id(it->block_id)));
    }

    return inner->block_writes(tmp, io_account, cb);
}


buf_ptr_t translator_serializer_t::block_read(const counted_t<standard_block_token_t> &token,
                                            file_account_t *io_account) {
    return inner->block_read(token, io_account);
}

counted_t<standard_block_token_t> translator_serializer_t::index_read(block_id_t block_id) {
    return inner->index_read(translate_block_id(block_id));
}

max_block_size_t translator_serializer_t::max_block_size() const {
    return inner->max_block_size();
}

bool translator_serializer_t::coop_lock_and_check() {
    return inner->coop_lock_and_check();
}

bool translator_serializer_t::is_gc_active() const {
    return inner->is_gc_active();
}

// A helper function for `end_block_id` and `end_aux_block_id`
// `first_block_id` is the lowest block ID in the range, either 0 for regular block
// IDs or FIRST_AUX_BLOCK_ID for aux blocks.
// `relative_inner_end_block_id` is the end block ID from the inner serializer,
// minus `first_block_id`.
block_id_t translator_serializer_t::compute_end_block_id(
        block_id_t first_block_id,
        block_id_t relative_inner_end_block_id) {
    int64_t x = relative_inner_end_block_id - cfgid.subsequent_ser_id();
    if (x <= 0) {
        x = 0;
    } else {
        while (x % mod_count != mod_id) x++;
        x /= mod_count;
    }

    block_id_t id = static_cast<block_id_t>(x) + first_block_id;
    rassert(translate_block_id(id) >= first_block_id + relative_inner_end_block_id);
    while (id > first_block_id) {
        --id;
        if (!get_delete_bit(id)) {
            ++id;
            break;
        }
    }
    return id;
}

block_id_t translator_serializer_t::end_block_id() {
    return compute_end_block_id(0, inner->end_block_id());
}

block_id_t translator_serializer_t::end_aux_block_id() {
    return compute_end_block_id(FIRST_AUX_BLOCK_ID,
                                make_aux_block_id_relative(inner->end_aux_block_id()));
}

segmented_vector_t<repli_timestamp_t>
translator_serializer_t::get_all_recencies(block_id_t first, block_id_t step) {
    return inner->get_all_recencies(translate_block_id(first),
                                    step * mod_count);
}

bool translator_serializer_t::get_delete_bit(block_id_t id) {
    return inner->get_delete_bit(translate_block_id(id));
}

void translator_serializer_t::offer_read_ahead_buf(
        block_id_t block_id,
        buf_ptr_t *buf,
        const counted_t<standard_block_token_t> &token) {
    inner->assert_thread();

    if (block_id <= CONFIG_BLOCK_ID.ser_id) {
        // Serializer multiplexer config block (or other blocks not untranslateable
        // to cache blocks) is not of interest.
        return;
    }

    // We only want the buffer if we are the correct shard.
    const int buf_mod_id = untranslate_block_id_to_mod_id(block_id, mod_count, cfgid);
    if (buf_mod_id != this->mod_id) {
        // We are not the correct shard.
        return;
    }

    // Okay, we take ownership of the buf, it's ours (even if read_ahead_callback is
    // nullptr).
    buf_ptr_t local_buf = std::move(*buf);

    if (read_ahead_callback != nullptr) {
        const block_id_t inner_block_id = untranslate_block_id_to_id(block_id, mod_count, mod_id, cfgid);
        read_ahead_callback->offer_read_ahead_buf(inner_block_id, &local_buf,
                                                  token);
    }
}

void translator_serializer_t::register_read_ahead_cb(serializer_read_ahead_callback_t *cb) {
    assert_thread();

    rassert(!read_ahead_callback);
    inner->register_read_ahead_cb(this);
    read_ahead_callback = cb;
}
void translator_serializer_t::unregister_read_ahead_cb(DEBUG_VAR serializer_read_ahead_callback_t *cb) {
    assert_thread();

    rassert(read_ahead_callback == nullptr || cb == read_ahead_callback);
    inner->unregister_read_ahead_cb(this);
    read_ahead_callback = nullptr;
}
