// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "serializer/merger.hpp"

#include <functional>

#include "errors.hpp"

#include "arch/runtime/coroutines.hpp"
#include "concurrency/new_mutex.hpp"
#include "config/args.hpp"
#include "serializer/types.hpp"

// TODO: Remove file