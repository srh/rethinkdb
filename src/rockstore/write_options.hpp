#ifndef RETHINKDB_ROCKSTORE_WRITE_OPTIONS_HPP_
#define RETHINKDB_ROCKSTORE_WRITE_OPTIONS_HPP_

namespace rockstore {

// Just to be nice to any includer that would declare this.
class store;

struct write_options {
    write_options() {}
    explicit write_options(bool _sync) : sync(_sync) {}
    bool sync = false;
    static write_options TODO() { return write_options(false); }
};

}

#endif  // RETHINKDB_ROCKSTORE_WRITE_OPTIONS_HPP_
