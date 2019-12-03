#include "clustering/immediate_consistency/version.hpp"

void debug_print(printf_buffer_t *buf, const version_t &v) {
    buf->appendf("v{");
    debug_print(buf, v.branch);
    buf->appendf(", ");
    debug_print(buf, v.timestamp);
    buf->appendf("}");
}