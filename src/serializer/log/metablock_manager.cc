// Copyright 2010-2013 RethinkDB, all rights reserved.
#include "serializer/log/metablock_manager.hpp"

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "arch/arch.hpp"
#include "arch/runtime/coroutines.hpp"
#include "concurrency/cond_var.hpp"
#include "serializer/log/log_serializer.hpp"
#include "version.hpp"

// TODO: Remove this file.