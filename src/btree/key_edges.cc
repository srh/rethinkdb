#include "btree/key_edges.hpp"

RDB_IMPL_SERIALIZABLE_2_FOR_CLUSTER(lower_key_bound, infinite, key);

std::string key_to_debug_str(const lower_key_bound &kb) {
    if (kb.infinite) {
        return "+inf";
    } else {
        return key_to_debug_str(kb.key);
    }
}

void debug_print(printf_buffer_t *buf, const lower_key_bound &kb) {
    if (kb.infinite) {
        buf->appendf("+inf");
    } else {
        debug_print(buf, kb.key);
    }
}

key_range_t::right_bound_t to_right_bound(const lower_key_bound &kb) {
    key_range_t::right_bound_t ret;
    ret.internal_key = kb.key;
    ret.unbounded = kb.infinite;
    return ret;
}

key_range_t half_open_key_range(const store_key_t &left, const lower_key_bound &right) {
    key_range_t ret;
    ret.left = left;
    ret.right = to_right_bound(right);
    return ret;
}

key_range_t to_key_range(const lower_key_bound_range &kr) {
    if (kr.is_empty()) {
        return key_range_t::empty();
    } else {
        return half_open_key_range(kr.left.key, kr.right);
    }
}

void debug_print(printf_buffer_t *buf, const lower_key_bound_range &kr) {
    buf->appendf("lower_key_bound_range(");
    debug_print(buf, kr.left);
    buf->appendf(", ");
    debug_print(buf, kr.right);
    buf->appendf(")");
}