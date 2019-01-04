// Copyright 2010-2014 RethinkDB, all rights reserved.
#ifndef CONTAINERS_DISK_BACKED_QUEUE_HPP_
#define CONTAINERS_DISK_BACKED_QUEUE_HPP_

#include <string>
#include <vector>

#include "concurrency/fifo_checker.hpp"
#include "concurrency/mutex.hpp"
#include "containers/buffer_group.hpp"
#include "containers/archive/buffer_group_stream.hpp"
#include "containers/archive/vector_stream.hpp"
#include "containers/scoped.hpp"
#include "perfmon/core.hpp"
#include "serializer/types.hpp"

class perfmon_collection_t;

class buffer_group_viewer_t {
public:
    virtual void view_buffer_group(const const_buffer_group_t *group) = 0;

protected:
    buffer_group_viewer_t() { }
    virtual ~buffer_group_viewer_t() { }

    DISABLE_COPYING(buffer_group_viewer_t);
};

class internal_disk_backed_queue_t {
public:
    internal_disk_backed_queue_t(
        const std::string &dirname,
        perfmon_collection_t *stats_parent);
    ~internal_disk_backed_queue_t();

    void push(const write_message_t &value);
    void push(const scoped_array_t<write_message_t> &values);

    void pop(buffer_group_viewer_t *viewer);

    bool empty();

    int64_t size();

    static constexpr int64_t file_block_size = 4 * MEGABYTE;
private:
    void push_single(const write_message_t &value);

    std::string block_filepath(int64_t block_id) const;

    void pop_bytes(std::vector<char> *onto, size_t length);

    mutex_t mutex;

    const std::string dirname_;
    bool created_directory_;

    perfmon_collection_t perfmon_collection_;
    perfmon_membership_t perfmon_membership_;

    // Number of elements in queue.
    int64_t queue_size_;

    // We always have a "tail block" in memory.  We read from it, and possibly
    // we write to it.

    // The end we pop from.  Always <= head_block_id_.
    int64_t tail_block_id_;
    size_t tail_buf_offset_;
    std::vector<char> tail_buf_;

    // The end we push onto.
    int64_t head_block_id_;
    // Only used if head_block_id_ > tail_block_id_.
    std::vector<char> head_buf_;

    // Precisely all the blocks with id less than tail_block_id_ and greater
    // than head_block_id_ are stored in files on disk.

    DISABLE_COPYING(internal_disk_backed_queue_t);
};

template <class T>
class deserializing_viewer_t : public buffer_group_viewer_t {
public:
    explicit deserializing_viewer_t(T *value_out) : value_out_(value_out) { }
    virtual ~deserializing_viewer_t() { }

    virtual void view_buffer_group(const const_buffer_group_t *group) {
        // TODO: We assume here that the data was serialized by _other_ code using
        // LATEST -- some in disk_backed_queue_t::push, but also in btree_store.cc,
        // which uses internal_disk_backed_queue_t directly.  (There's no good reason
        // for this today: it needed to be generic when that code was templatized on
        // protocol_t.)
        deserialize_from_group<cluster_version_t::LATEST_OVERALL>(group, value_out_);
    }

private:
    T *value_out_;

    DISABLE_COPYING(deserializing_viewer_t);
};

// Copies the buffer group into a write_message_t
class copying_viewer_t : public buffer_group_viewer_t {
public:
    explicit copying_viewer_t(write_message_t *wm_out) : wm_out_(wm_out) { }
    ~copying_viewer_t() { }

    void view_buffer_group(const const_buffer_group_t *group) {
        buffer_group_read_stream_t stream(group);
        char buf[1024];
        while (!stream.entire_stream_consumed()) {
            int64_t c = stream.read(&buf, 1024);
            wm_out_->append(&buf, c);
        }
    }

private:
    write_message_t *wm_out_;

    DISABLE_COPYING(copying_viewer_t);
};

template <class T>
class disk_backed_queue_t {
public:
    disk_backed_queue_t(
            const std::string &dirname,
            perfmon_collection_t *stats_parent)
        : internal_(dirname, stats_parent) { }

    void push(const T &t) {
        // TODO: There's an unnecessary copying of data here (which would require a
        // serialization_size overloaded function to be implemented in order to eliminate).
        // TODO: We have such a serialization_size function.
        write_message_t wm;
        // Despite that we are serializing this *to disk* disk backed
        // queues are not intended to persist across restarts, so this
        // is safe.
        serialize<cluster_version_t::LATEST_OVERALL>(&wm, t);
        internal_.push(wm);
    }

    void pop(T *out) {
        deserializing_viewer_t<T> viewer(out);
        internal_.pop(&viewer);
    }

    bool empty() {
        return internal_.empty();
    }

    int64_t size() {
        return internal_.size();
    }

private:
    internal_disk_backed_queue_t internal_;
    DISABLE_COPYING(disk_backed_queue_t);
};

#endif /* CONTAINERS_DISK_BACKED_QUEUE_HPP_ */
