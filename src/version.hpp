#ifndef VERSION_HPP_
#define VERSION_HPP_

// An enumeration of the current and previous recognized versions.  Every
// serialization / deserialization happens relative to one of these versions.
enum class obsolete_cluster_version_t {
    v1_13 = 0,
    v1_13_2 = 1,

    v1_13_2_is_latest = v1_13_2
};
enum class cluster_version_t {
    // Beware that adding/removing new values requires updates to the serialization
    // function.
    v1_14 = 2,
    v1_15 = 3,
    v1_16 = 4,
    v2_0 = 5,
    v2_1 = 6,
    v2_2 = 7,
    v2_3 = 8,
    v2_4 = 9,

    // The v2.3-ext serialization format is only used over the cluster.  We keep the
    // disk format backwards compatible with the official v2.3 releases -- extra
    // information (which we will add to table_config_t) will get stored "out of band."

    // We never use the v2.3-ext format in this version (2.4-ext) of the software.

    // v2_3_ext = 0x7f - v2_3,

    // Likewise with v2_4.
    v2_4_ext = 0x7f - v2_4,

    // This is used in places where _something_ needs to change when a new cluster
    // version is created.  (Template instantiations, switches on version number,
    // etc.)
    v2_4_ext_is_latest = v2_4_ext,

    // Like the *_is_latest version, but for code that's only concerned with disk
    // serialization. Must be changed whenever LATEST_DISK gets changed.
    v2_4_is_latest_disk = v2_4,

    // The latest version, max of CLUSTER and LATEST_DISK
    LATEST_OVERALL = v2_4_ext_is_latest,

    // The latest version for disk serialization can sometimes be different from the
    // version we use for cluster serialization.  This is also the latest version of
    // ReQL deterministic function behavior.
    LATEST_DISK = v2_4,

    // This exists as long as the clustering code only supports the use of one
    // version.  It uses cluster_version_t::CLUSTER wherever it uses this.
    CLUSTER = LATEST_OVERALL,
};

// Uncomment this if cluster_version_t::LATEST_DISK == cluster_version_t::CLUSTER.
// Comment it otherwise. This macro is used to avoid instantiating the same version
// twice in the `INSTANTIATE_SERIALIZE_FOR_CLUSTER_AND_DISK` macro.

// #define CLUSTER_AND_DISK_VERSIONS_ARE_SAME

#ifdef CLUSTER_AND_DISK_VERSIONS_ARE_SAME
static_assert(cluster_version_t::CLUSTER == cluster_version_t::LATEST_DISK,
              "Comment #define CLUSTER_AND_DISK_VERSIONS_ARE_SAME in version.hpp");
#else
static_assert(cluster_version_t::CLUSTER != cluster_version_t::LATEST_DISK,
              "Uncomment #define CLUSTER_AND_DISK_VERSIONS_ARE_SAME in version.hpp");
#endif

// We will not (barring a bug) even attempt to deserialize a version number that we
// do not support.  Cluster nodes connecting to us make sure that they are
// communicating with a version number that we also support (the max of our two
// versions) and we don't end up deserializing version numbers in that situation
// anyway (yet).  Files on disk will not (barring corruption, or a bug) cause us to
// try to use a version number that we do not support -- we'll see manually whether
// we can read the serializer file by looking at the disk_format_version field in the
// metablock.
//
// At some point the set of cluster versions and disk versions that we support might
// diverge.  It's likely that we'd support a larger window of serialization versions in
// the on-disk format.  Or maybe, the cluster version marches forward, but as a
// compatibility hack, the disk version remains the same.
//
// Also, note: it's possible that versions will not be linearly ordered: Suppose we
// release v1.17 and then v1.18.  Perhaps v1.17 supports v1_16 and v1_17 and v1.18
// supports v1_17 and v1_18.  Suppose we then need to make a point-release, v1.17.1,
// that changes a serialization format, and we add a new cluster version v1_17_1.
// Then, v1.18 would still only support v1_17 and v1_18.  However, v1.18.1 might
// support v1_17, v1_17_1, and v1_18 (and v1_18_1 if that needs to be created).


enum class obsolete_reql_version_t {
    v1_13 = 0,
    v1_14 = 1,
    v1_15 = v1_14,

    v1_15_is_latest = v1_15,

    EARLIEST = v1_13,
    LATEST = v1_15_is_latest
};

// Reql versions define how secondary index functions should be evaluated.  Older
// versions have bugs that are fixed in newer versions.  They also define how secondary
// index keys are generated.
enum class reql_version_t {
    v1_16 = 2,
    v2_0 = 3,
    v2_1 = 4,
    v2_2 = 5,
    v2_3 = 6,
    v2_4 = 7,

    // Code that uses _is_latest may need to be updated when the
    // version changes
    v2_4_is_latest = v2_4,

    EARLIEST = v1_16,
    LATEST = v2_4_is_latest
};

// Serialization of reql_version_t is defined in protocol_api.hpp.

#endif  // VERSION_HPP_
