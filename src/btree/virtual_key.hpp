// Copyright 2010-2019 RethinkDB, all rights reserved.
#ifndef BTREE_VIRTUAL_KEY_HPP_
#define BTREE_VIRTUAL_KEY_HPP_

#include "btree/keys.hpp"

// Stores either a key, or a value representing the decrementation of said key.
struct virtual_key {
    bool is_decremented = false;
    store_key_t key;

    virtual_key(bool _is_decremented, store_key_t _key)
        : is_decremented(_is_decremented), key(_key) {}
    explicit virtual_key(store_key_t&& k) : is_decremented(false), key(std::move(k)) {}
    static virtual_key decremented(store_key_t&& k) {
        return virtual_key(true, std::move(k));
    }

    static virtual_key guarantee_decremented(store_key_t&& k) {
        guarantee(k.size() != 0, "guarantee_decremented sees empty key");
        return virtual_key(true, std::move(k));
    }


    bool operator>=(const store_key_t &rhs) const {
        return is_decremented ? key > rhs : key >= rhs;
    }
};


#endif  // BTREE_VIRTUAL_KEY_HPP_
