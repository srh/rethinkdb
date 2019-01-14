// Copyright 2010-2013 RethinkDB, all rights reserved.
#include "serializer/log/static_header.hpp"

#include <functional>
#include <vector>

#include "arch/arch.hpp"
#include "arch/runtime/coroutines.hpp"
#include "containers/scoped.hpp"
#include "config/args.hpp"
#include "logger.hpp"
#include "utils.hpp"
