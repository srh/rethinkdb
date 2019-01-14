// Copyright 2010-2014 RethinkDB, all rights reserved.
#ifndef SERIALIZER_LOG_LOG_SERIALIZER_HPP_
#define SERIALIZER_LOG_LOG_SERIALIZER_HPP_

#include <map>
#include <string>
#include <vector>
#include <list>

#include "arch/compiler.hpp"
#include "arch/types.hpp"
#include "serializer/serializer.hpp"
#include "serializer/log/config.hpp"
#include "utils.hpp"
#include "concurrency/mutex_assertion.hpp"
#include "concurrency/new_mutex.hpp"
#include "concurrency/signal.hpp"
#include "concurrency/cond_var.hpp"
#include "containers/scoped.hpp"
#include "paths.hpp"
#include "serializer/log/metablock_manager.hpp"
#include "serializer/log/extent_manager.hpp"
#include "serializer/log/stats.hpp"

class cond_t;
struct block_magic_t;
class io_backender_t;

// TODO: Remove this file.

#endif /* SERIALIZER_LOG_LOG_SERIALIZER_HPP_ */
