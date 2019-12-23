#include "btree/key_edges.hpp"

RDB_IMPL_SERIALIZABLE_2_FOR_CLUSTER(key_or_max, infinite, key);

std::string key_to_debug_str(const key_or_max &km) {
    if (km.infinite) {
        return "+inf";
    } else {
        return key_to_debug_str(km.key);
    }
}

void debug_print(printf_buffer_t *buf, const key_or_max &km) {
    if (km.infinite) {
        buf->appendf("+inf");
    } else {
        debug_print(buf, km.key);
    }
}

key_range_t half_open_key_range(store_key_t left, lower_key_bound right) {
    key_range_t ret;
    ret.left = std::move(left);
    ret.right = std::move(right).to_right_bound();
    return ret;
}

key_range_t to_key_range(lower_key_bound_range kr) {
    if (kr.is_empty()) {
        return key_range_t::empty();
    } else {
        return half_open_key_range(std::move(kr.left.key), std::move(kr.right));
    }
}

void debug_print(printf_buffer_t *buf, const lower_key_bound_range &kr) {
    buf->appendf("lower_key_bound_range(");
    debug_print(buf, kr.left);
    buf->appendf(", ");
    debug_print(buf, kr.right);
    buf->appendf(")");
}