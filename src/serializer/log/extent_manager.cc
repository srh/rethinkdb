// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "serializer/log/extent_manager.hpp"

#include <queue>

#include "arch/arch.hpp"
#include "logger.hpp"
#include "math.hpp"
#include "perfmon/perfmon.hpp"
#include "serializer/log/log_serializer.hpp"

// TODO: Remove this file.