// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "buffer_cache/stats.hpp"

#include "perfmon/perfmon.hpp"
#include "rdb_protocol/datum.hpp"

// TODO: We used to have an in_use_bytes stat... maybe that's worth harvesting from rocksdb.
