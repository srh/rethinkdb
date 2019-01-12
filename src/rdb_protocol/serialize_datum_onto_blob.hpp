#ifndef RDB_PROTOCOL_SERIALIZE_DATUM_ONTO_BLOB_HPP_
#define RDB_PROTOCOL_SERIALIZE_DATUM_ONTO_BLOB_HPP_

#include "containers/buffer_group.hpp"
#include "rdb_protocol/serialize_datum.hpp"

inline void datum_deserialize_from_group(const const_buffer_group_t *group,
                                         ql::datum_t *value_out) {
    buffer_group_read_stream_t stream(group);
    archive_result_t res = datum_deserialize(&stream, value_out);
    guarantee_deserialization(res, "datum_t (from a buffer group)");
    guarantee(stream.entire_stream_consumed(),
              "Corrupted value in storage (deserialization terminated early).");
}

// The name datum_deserialize_from_buf was taken.
inline ql::datum_t datum_deserialize_from_vec(const void *buf, size_t count) {
    buffer_group_t group;
    group.add_buffer(count, buf);
    ql::datum_t ret;
    datum_deserialize_from_group(const_view(&group), &ret);
    return ret;
}


#endif /* RDB_PROTOCOL_SERIALIZE_DATUM_ONTO_BLOB_HPP_ */
