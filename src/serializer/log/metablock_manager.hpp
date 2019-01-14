// Copyright 2010-2012 RethinkDB, all rights reserved.
#ifndef SERIALIZER_LOG_METABLOCK_MANAGER_HPP_
#define SERIALIZER_LOG_METABLOCK_MANAGER_HPP_

/* Notice:
 * This file defines templatized classes and does not provide their
 * implementations. Those implementations are provided in
 * serializer/log/metablock_manager.tcc which is included by
 * serializer/log/metablock_manager.cc and explicitly instantiates the template
 * on the type log_serializer_metablock_t. If you want to use this type as the
 * template parameter you can do so and it should work fine... if you want to
 * use a different type you may have some difficulty and should look at what's
 * going on in the aforementioned files. */

#include <stddef.h>
#include <string.h>

#include <vector>

#include "errors.hpp"
#include <boost/crc.hpp>

#include "arch/compiler.hpp"
#include "arch/types.hpp"
#include "concurrency/mutex.hpp"
#include "serializer/log/extent_manager.hpp"
#include "serializer/log/static_header.hpp"


#endif /* SERIALIZER_LOG_METABLOCK_MANAGER_HPP_ */
