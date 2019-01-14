#ifndef BUFFER_CACHE_CACHE_ACCOUNT_HPP_
#define BUFFER_CACHE_CACHE_ACCOUNT_HPP_

#include "errors.hpp"

// TODO: This is kind of vestigial, figure out what to do with callers/users,
// figure out how this might work with rocksdb.

class cache_account_t {
public:
    cache_account_t() = default;
    ~cache_account_t() = default;
    cache_account_t(cache_account_t &&movee) = default;
    cache_account_t &operator=(cache_account_t &&movee) = default;

    DISABLE_COPYING(cache_account_t);
};

#endif  // BUFFER_CACHE_CACHE_ACCOUNT_HPP_
