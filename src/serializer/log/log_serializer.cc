// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "serializer/log/log_serializer.hpp"

#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <functional>

#include "arch/io/disk.hpp"
#include "arch/runtime/runtime.hpp"
#include "arch/runtime/coroutines.hpp"
#include "buffer_cache/types.hpp"
#include "concurrency/new_mutex.hpp"
#include "logger.hpp"
#include "perfmon/perfmon.hpp"
#include "serializer/buf_ptr.hpp"
#include "serializer/log/data_block_manager.hpp"
