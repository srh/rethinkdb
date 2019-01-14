#include "buffer_cache/cache_balancer.hpp"

#include <algorithm>
#include <limits>

#include "buffer_cache/evicter.hpp"
#include "arch/runtime/runtime.hpp"
#include "concurrency/pmap.hpp"

// TODO: Remove this file.