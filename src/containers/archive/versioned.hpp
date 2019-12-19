#ifndef CONTAINERS_ARCHIVE_VERSIONED_HPP_
#define CONTAINERS_ARCHIVE_VERSIONED_HPP_

#include "containers/archive/archive.hpp"
#include "version.hpp"


namespace archive_internal {

class bogus_made_up_type_t;

}  // namespace archive_internal

// These are generally universal.  They must not have their behavior change -- except
// if we remove some cluster_version_t value, in which case... maybe would fail on a
// range error with the specific removed values.  Or maybe, we would do something
// differently.
inline void serialize_cluster_version(write_message_t *wm, cluster_version_t v) {
    int8_t raw = static_cast<int8_t>(v);
    serialize<cluster_version_t::LATEST_OVERALL>(wm, raw);
}

// Same values as archive_result_t.  Maybe UNRECOGNIZED_VERSION should get folded
// into RANGE_ERROR (which is impossible when deserializing an INT8, fwiw).
enum class cluster_version_result_t {
    SUCCESS,
    SOCK_ERROR,
    SOCK_EOF,
    INT8_RANGE_ERROR,
    OBSOLETE_VERSION,  // This would call obsolete_cb which would crash or throw.
    UNRECOGNIZED_VERSION,  // This previously threw an archive_exc_t.
};

struct reql_version_result_t {
    cluster_version_result_t code;
    // If code is OBSOLETE_VERSION, this field gets used.
    obsolete_reql_version_t obsolete_version;
};

MUST_USE cluster_version_result_t deserialize_cluster_version(
        read_stream_t *s,
        cluster_version_t *out) noexcept;

MUST_USE reql_version_result_t deserialize_importable_reql_version(
        read_stream_t *s, importable_reql_version_t *thing);

template <class T>
void serialize_for_cluster(write_message_t *wm, const T &value) {
    serialize<cluster_version_t::CLUSTER>(wm, value);
}

// Deserializes a value, assuming it's serialized for a given version.  (This doesn't
// deserialize any version numbers.)
template <class T>
archive_result_t deserialize_for_version(cluster_version_t version,
                                         read_stream_t *s,
                                         T *thing) {
    switch (version) {
    case cluster_version_t::v1_14:
        return deserialize<cluster_version_t::v1_14>(s, thing);
    case cluster_version_t::v1_15:
        return deserialize<cluster_version_t::v1_15>(s, thing);
    case cluster_version_t::v1_16:
        return deserialize<cluster_version_t::v1_16>(s, thing);
    case cluster_version_t::v2_0:
        return deserialize<cluster_version_t::v2_0>(s, thing);
    case cluster_version_t::v2_1:
        return deserialize<cluster_version_t::v2_1>(s, thing);
    case cluster_version_t::v2_2:
        return deserialize<cluster_version_t::v2_2>(s, thing);
    case cluster_version_t::v2_3:
        return deserialize<cluster_version_t::v2_3>(s, thing);
    case cluster_version_t::v2_4:
        return deserialize<cluster_version_t::v2_4>(s, thing);
    case cluster_version_t::v2_5_is_latest:
        return deserialize<cluster_version_t::v2_5_is_latest>(s, thing);
    default:
        unreachable("deserialize_for_version: unsupported cluster version");
    }
}

// Some serialized_size needs to be visible, apparently, so that
// serialized_size_for_version will actually parse.
template <cluster_version_t W>
size_t serialized_size(const archive_internal::bogus_made_up_type_t &);

template <class T>
size_t serialized_size_for_version(cluster_version_t version,
                                   const T &thing) {
    switch (version) {
    case cluster_version_t::v1_14:
        return serialized_size<cluster_version_t::v1_14>(thing);
    case cluster_version_t::v1_15:
        return serialized_size<cluster_version_t::v1_15>(thing);
    case cluster_version_t::v1_16:
        return serialized_size<cluster_version_t::v1_16>(thing);
    case cluster_version_t::v2_0:
        return serialized_size<cluster_version_t::v2_0>(thing);
    case cluster_version_t::v2_1:
        return serialized_size<cluster_version_t::v2_1>(thing);
    case cluster_version_t::v2_2:
        return serialized_size<cluster_version_t::v2_2>(thing);
    case cluster_version_t::v2_3:
        return serialized_size<cluster_version_t::v2_3>(thing);
    case cluster_version_t::v2_4:
        return serialized_size<cluster_version_t::v2_4>(thing);
    case cluster_version_t::v2_5_is_latest:
        return serialized_size<cluster_version_t::v2_5_is_latest>(thing);
    default:
        unreachable("serialize_size_for_version: unsupported version");
    }
}

// We want to express explicitly whether a given serialization function
// is used for cluster messages or disk serialization in case the latest cluster
// and latest disk versions diverge.
//
// If you see either the INSTANTIATE_SERIALIZE_FOR_CLUSTER_AND_DISK
// of INSTANTIATE_SERIALIZE_FOR_DISK macro used somewhere, you know that if you
// change the serialization format of that type that will break the disk format,
// and you should consider writing a deserialize function for the older versions.

#define INSTANTIATE_SERIALIZE_FOR_DISK(typ)                  \
    template void serialize<cluster_version_t::LATEST_DISK>( \
            write_message_t *, const typ &)

#define INSTANTIATE_SERIALIZE_FOR_CLUSTER(typ)           \
    template void serialize<cluster_version_t::CLUSTER>( \
            write_message_t *, const typ &)

#define INSTANTIATE_DESERIALIZE_FOR_CLUSTER(typ)                       \
    template archive_result_t deserialize<cluster_version_t::CLUSTER>( \
                            read_stream_t *, typ *)

#ifdef CLUSTER_AND_DISK_VERSIONS_ARE_SAME
#define INSTANTIATE_SERIALIZE_FOR_CLUSTER_AND_DISK(typ)  \
    template void serialize<cluster_version_t::CLUSTER>( \
            write_message_t *, const typ &)
#else
#define INSTANTIATE_SERIALIZE_FOR_CLUSTER_AND_DISK(typ)      \
    template void serialize<cluster_version_t::CLUSTER>(     \
            write_message_t *, const typ &);                 \
    template void serialize<cluster_version_t::LATEST_DISK>( \
            write_message_t *, const typ &)
#endif

#define INSTANTIATE_SERIALIZABLE_FOR_CLUSTER(typ)                      \
    INSTANTIATE_SERIALIZE_FOR_CLUSTER(typ);                            \
    template archive_result_t deserialize<cluster_version_t::CLUSTER>( \
            read_stream_t *, typ *)


#define INSTANTIATE_DESERIALIZE_SINCE_v2_5(typ) \
    template archive_result_t deserialize<cluster_version_t::v2_5_is_latest>( \
            read_stream_t *, typ *)

#define INSTANTIATE_SERIALIZABLE_SINCE_v2_4(typ) \
    INSTANTIATE_SERIALIZE_FOR_CLUSTER_AND_DISK(typ); \
    INSTANTIATE_DESERIALIZE_SINCE_v2_4(typ)


#define INSTANTIATE_DESERIALIZE_SINCE_v2_4(typ) \
    template archive_result_t deserialize<cluster_version_t::v2_4>( \
            read_stream_t *, typ *); \
    INSTANTIATE_DESERIALIZE_SINCE_v2_5(typ)

#define INSTANTIATE_SERIALIZABLE_SINCE_v2_4(typ)         \
    INSTANTIATE_SERIALIZE_FOR_CLUSTER_AND_DISK(typ);     \
    INSTANTIATE_DESERIALIZE_SINCE_v2_4(typ)

#define INSTANTIATE_DESERIALIZE_SINCE_v2_3(typ)                                  \
    template archive_result_t deserialize<cluster_version_t::v2_3>(              \
            read_stream_t *, typ *);                                             \
    INSTANTIATE_DESERIALIZE_SINCE_v2_4(typ)

#define INSTANTIATE_SERIALIZABLE_SINCE_v2_3(typ)         \
    INSTANTIATE_SERIALIZE_FOR_CLUSTER_AND_DISK(typ);     \
    INSTANTIATE_DESERIALIZE_SINCE_v2_3(typ)

#define INSTANTIATE_DESERIALIZE_SINCE_v2_2(typ)                                  \
    template archive_result_t deserialize<cluster_version_t::v2_2>(              \
            read_stream_t *, typ *);                                             \
    INSTANTIATE_DESERIALIZE_SINCE_v2_3(typ)

#define INSTANTIATE_SERIALIZABLE_SINCE_v2_2(typ)         \
    INSTANTIATE_SERIALIZE_FOR_CLUSTER_AND_DISK(typ);     \
    INSTANTIATE_DESERIALIZE_SINCE_v2_2(typ)

#define INSTANTIATE_DESERIALIZE_SINCE_v2_1(typ)                                  \
    template archive_result_t deserialize<cluster_version_t::v2_1>(              \
            read_stream_t *, typ *);                                             \
    INSTANTIATE_DESERIALIZE_SINCE_v2_2(typ)

#define INSTANTIATE_SERIALIZABLE_SINCE_v2_1(typ)         \
    INSTANTIATE_SERIALIZE_FOR_CLUSTER_AND_DISK(typ);     \
    INSTANTIATE_DESERIALIZE_SINCE_v2_1(typ)

#define INSTANTIATE_DESERIALIZE_SINCE_v1_16(typ)                                 \
    template archive_result_t deserialize<cluster_version_t::v1_16>(             \
            read_stream_t *, typ *);                                             \
    template archive_result_t deserialize<cluster_version_t::v2_0>(              \
            read_stream_t *, typ *);                                             \
    INSTANTIATE_DESERIALIZE_SINCE_v2_1(typ)

#define INSTANTIATE_SERIALIZABLE_SINCE_v1_16(typ)        \
    INSTANTIATE_SERIALIZE_FOR_CLUSTER_AND_DISK(typ);     \
    INSTANTIATE_DESERIALIZE_SINCE_v1_16(typ)

#define INSTANTIATE_DESERIALIZE_SINCE_v1_13(typ)                                 \
    template archive_result_t deserialize<cluster_version_t::v1_14>(             \
            read_stream_t *, typ *);                                             \
    template archive_result_t deserialize<cluster_version_t::v1_15>(             \
            read_stream_t *, typ *);                                             \
    INSTANTIATE_DESERIALIZE_SINCE_v1_16(typ)

#define INSTANTIATE_SERIALIZABLE_SINCE_v1_13(typ)        \
    INSTANTIATE_SERIALIZE_FOR_CLUSTER_AND_DISK(typ);     \
    INSTANTIATE_DESERIALIZE_SINCE_v1_13(typ)




#endif  // CONTAINERS_ARCHIVE_VERSIONED_HPP_
