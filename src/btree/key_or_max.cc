#include "btree/key_or_max.hpp"

RDB_IMPL_SERIALIZABLE_2_FOR_CLUSTER(key_or_max, infinite, key);

void debug_print(printf_buffer_t *buf, const key_or_max &km) {
    if (km.infinite) {
        buf->appendf("+inf");
    } else {
        debug_print(buf, km.key);
    }
}
