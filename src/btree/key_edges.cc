#include "btree/key_edges.hpp"

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

void debug_print(printf_buffer_t *buf, const lower_key_bound_range &kr) {
    buf->appendf("lower_key_bound_range(");
    debug_print(buf, kr.left);
    buf->appendf(", ");
    debug_print(buf, kr.right);
    buf->appendf(")");
}
