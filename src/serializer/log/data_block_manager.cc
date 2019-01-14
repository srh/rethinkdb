// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "serializer/log/data_block_manager.hpp"

#include <inttypes.h>
#include <sys/uio.h>

#include <functional>

#include "arch/arch.hpp"
#include "arch/runtime/coroutines.hpp"
#include "concurrency/mutex.hpp"
#include "concurrency/new_mutex.hpp"
#include "errors.hpp"
#include "perfmon/perfmon.hpp"
#include "serializer/buf_ptr.hpp"
#include "serializer/log/log_serializer.hpp"
#include "stl_utils.hpp"

// TODO: Remove this file