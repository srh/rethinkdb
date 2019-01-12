// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "containers/disk_backed_queue.hpp"

#include "arch/io/disk.hpp"
#include "arch/runtime/thread_pool.hpp"
#include "buffer_cache/alt.hpp"
#include "buffer_cache/cache_balancer.hpp"
#include "paths.hpp"
#include "serializer/log/log_serializer.hpp"

void create_directory(const std::string &dirname) {
    linux_thread_pool_t::run_in_blocker_pool([&]() {
        int res = ::mkdir(dirname.c_str(), 0755);
        guarantee_err(
            res == 0,
            "Disk backed queue could not make directory '%s'",
            dirname.c_str());
    });
}

void remove_directory(const std::string &dirname) {
    linux_thread_pool_t::run_in_blocker_pool([&]() {
        remove_directory_recursive(dirname.c_str());
    });
}

internal_disk_backed_queue_t::internal_disk_backed_queue_t(
        const std::string &dirname,
        perfmon_collection_t *stats_parent)
    : dirname_(dirname),
      created_directory_(false),
      perfmon_membership_(stats_parent, &perfmon_collection_,
                          dirname_.c_str()),
      queue_size_(0),
      tail_block_id_(0),
      tail_buf_offset_(0),
      head_block_id_(0) {
}

internal_disk_backed_queue_t::~internal_disk_backed_queue_t() {
    if (created_directory_) {
        remove_directory(dirname_);
    }
}

void internal_disk_backed_queue_t::push(const write_message_t &wm) {
    mutex_t::acq_t mutex_acq(&mutex);
    push_single(wm);
}

void internal_disk_backed_queue_t::push(const scoped_array_t<write_message_t> &wms) {
    mutex_t::acq_t mutex_acq(&mutex);

    for (size_t i = 0; i < wms.size(); ++i) {
        push_single(wms[i]);
    }
}

bool append_upto_max(
        std::vector<char> *onto, const std::vector<char> &msg,
        size_t *msg_offset, size_t max_onto_size) {
    size_t to_append = std::min<size_t>(max_onto_size - onto->size(), msg.size() - *msg_offset);
    onto->insert(onto->end(), &msg[*msg_offset], &msg[*msg_offset + to_append]);
    *msg_offset += to_append;
    return *msg_offset == msg.size();
}

// Returns number of bytes read.
size_t read_upto_count(
        std::vector<char> *onto, const std::vector<char> &from,
        size_t *from_offset, size_t count) {
    size_t to_read = std::min<size_t>(from.size() - *from_offset, count);
    onto->insert(onto->end(), &from[*from_offset], &from[*from_offset + to_read]);
    *from_offset += to_read;
    return to_read;
}

void write_file(const std::string &path, const std::vector<char> &buf) {
    scoped_fd_t fd = io_utils::create_file(path.c_str());
    std::string error_msg;
    if (!io_utils::write_all(fd.get(), buf.data(), buf.size(), &error_msg)) {
        throw std::runtime_error(
            strprintf("Writing disk backed queue file '%s' failed: %s",
                path.c_str(), error_msg.c_str()));
    }
}

std::vector<char> read_file(const std::string &path) {
    std::string error_msg;
    scoped_fd_t fd = io_utils::open_file_for_read(path.c_str(), &error_msg);
    if (fd.get() == INVALID_FD) {
        throw std::runtime_error(
            strprintf("Opening disk backed queue file '%s' failed: %s",
                path.c_str(), error_msg.c_str()));
    }

    std::vector<char> buf;
    scoped_array_t<char> array(internal_disk_backed_queue_t::file_block_size);
    while (buf.size() < internal_disk_backed_queue_t::file_block_size) {
        ssize_t res;
        do {
            res = ::pread(fd.get(), array.data(), array.size(), buf.size());
        } while (res == -1 && get_errno() == EINTR);
        if (res == -1) {
            throw std::runtime_error(
                strprintf("Opening disk backed queue file '%s' failed: %s",
                    path.c_str(), errno_string(get_errno()).c_str()));
        }
        guarantee(res != 0, "File should have size file_block_size.");
        buf.insert(buf.end(), array.data(), array.data() + res);
    }

    return buf;
}

std::vector<char> read_and_delete_file(const std::string &path) {
    std::vector<char> contents = io_utils::read_file(path.c_str());
    guarantee(contents.size() == internal_disk_backed_queue_t::file_block_size);
    io_utils::delete_file(path.c_str());
    return contents;
}

std::string internal_disk_backed_queue_t::block_filepath(int64_t block_id) const {
    return dirname_ + strprintf(PATH_SEPARATOR "%" PRIi64 ".dbq", block_id);
}

void internal_disk_backed_queue_t::push_single(const write_message_t &wm) {
    // This runs in the blocker pool.
    vector_stream_t vecs;
    size_t wm_size = wm.size();
    DEBUG_VAR int64_t res = vecs.write(&wm_size, sizeof(wm_size));
    rassert(res >= 0);
    DEBUG_VAR int res2 = send_write_message(&vecs, &wm);
    rassert(res2 == 0);

    std::vector<char> msg = std::move(vecs.vector());
    size_t msg_offset = 0;

    if (head_block_id_ == tail_block_id_) {
        if (append_upto_max(&tail_buf_, msg, &msg_offset, file_block_size)) {
            goto done;
        }
        ++head_block_id_;
        rassert(head_buf_.empty());
        head_buf_.reserve(file_block_size);
    }
    for (;;) {
        if (append_upto_max(&head_buf_, msg, &msg_offset, file_block_size)) {
            goto done;
        }

        linux_thread_pool_t::run_in_blocker_pool([&]() {
            // Write head file.
            if (!created_directory_) {
                create_directory(dirname_);
                created_directory_ = true;
            }
            std::string path = block_filepath(head_block_id_);
            write_file(path, head_buf_);

            ++head_block_id_;
            head_buf_.clear();
            head_buf_.reserve(file_block_size);
        });
    }

done:
    ++queue_size_;
}

void internal_disk_backed_queue_t::pop(buffer_group_viewer_t *viewer) {
    guarantee(size() != 0);
    mutex_t::acq_t mutex_acq(&mutex);
    std::vector<char> message;
    linux_thread_pool_t::run_in_blocker_pool([&]() {
        std::vector<char> size;
        pop_bytes(&size, sizeof(size_t));
        size_t wm_size = *reinterpret_cast<size_t *>(size.data());
        pop_bytes(&message, wm_size);
    });

    const_buffer_group_t group;
    group.add_buffer(message.size(), message.data());

    viewer->view_buffer_group(&group);
    --queue_size_;
}

void internal_disk_backed_queue_t::pop_bytes(std::vector<char> *onto, size_t length) {
    // Runs in blocker pool.
    for (;;) {
        size_t n = read_upto_count(onto, tail_buf_, &tail_buf_offset_, length);
        if (tail_buf_offset_ == file_block_size) {
            tail_buf_offset_ = 0;
            if (tail_block_id_ == head_block_id_) {
                ++tail_block_id_;
                ++head_block_id_;
                tail_buf_.clear();
                rassert(empty());
            } else if (tail_block_id_ + 1 == head_block_id_) {
                ++tail_block_id_;
                tail_buf_.clear();
                // Use swap in place of tail_buf_ = std::move(head_buf_), to avoid
                // deallocating buffers.
                std::swap(tail_buf_, head_buf_);
            } else {
                ++tail_block_id_;
                std::string path = block_filepath(tail_block_id_);
                linux_thread_pool_t::run_in_blocker_pool([&]() {
                    tail_buf_ = read_and_delete_file(path);
                });
            }
        }

        length -= n;
        if (length == 0) {
            --queue_size_;
            return;
        }
    }
}

bool internal_disk_backed_queue_t::empty() {
    return queue_size_ == 0;
}

int64_t internal_disk_backed_queue_t::size() {
    return queue_size_;
}


