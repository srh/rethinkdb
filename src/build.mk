# Copyright 2010-2013 RethinkDB, all rights reserved.

##### Build parameters

# We assemble path directives.
LDFLAGS ?=
CXXFLAGS ?=
RT_LDFLAGS = $(LDFLAGS) $(RE2_LIBS) $(TERMCAP_LIBS) $(Z_LIBS) $(CURL_LIBS) $(CRYPTO_LIBS) $(SSL_LIBS)
RT_LDFLAGS += $(V8_LIBS) $(PROTOBUF_LIBS) $(PTHREAD_LIBS) $(MALLOC_LIBS)
RT_CXXFLAGS := $(CXXFLAGS) $(RE2_INCLUDE) $(V8_INCLUDE) $(PROTOBUF_INCLUDE) $(BOOST_INCLUDE) $(Z_INCLUDE) $(CURL_INCLUDE) $(CRYPTO_INCLUDE)
ALL_INCLUDE_DEPS := $(RE2_INCLUDE_DEP) $(V8_INCLUDE_DEP) $(PROTOBUF_INCLUDE_DEP) $(BOOST_INCLUDE_DEP) $(Z_INCLUDE_DEP) $(CURL_INCLUDE_DEP) $(CRYPTO_INCLUDE_DEP) $(SSL_INCLUDE_DEP)

ifeq ($(USE_CCACHE),1)
  RT_CXX := ccache $(CXX)
  ifeq ($(COMPILER),CLANG)
    RT_CXXFLAGS += -Qunused-arguments
  endif
else
  RT_CXX := $(CXX)
endif

STATICFORCE := $(STATIC)

ifeq ($(COMPILER),CLANG)

  ifeq ($(OS),Darwin)
    # TODO: ld: unknown option: --no-as-needed
    # RT_LDFLAGS += -Wl,--no-as-needed
  endif

  ifeq ($(STATICFORCE),1)
    # TODO(OSX)
    ifeq ($(OS),Linux)
      RT_LDFLAGS += -static
      STATIC_LIBGCC := 1
    endif
  endif

  RT_LDFLAGS += $(M_LIBS)

else ifeq ($(COMPILER),INTEL)
  RT_LDFLAGS += -B/opt/intel/bin

  ifeq ($(STATICFORCE),1)
    # TODO(OSX)
    ifeq ($(OS),Linux)
      RT_LDFLAGS += -static
      STATIC_LIBGCC = 1
    endif
  endif

  RT_LDFLAGS += -lstdc++
else ifeq ($(COMPILER),GCC)

  ifeq ($(OS),Linux)
    RT_LDFLAGS += -Wl,--no-as-needed
  endif

  ifeq ($(STATICFORCE),1)
    # TODO(OSX)
    ifeq ($(OS),Linux)
      RT_LDFLAGS += -static
      STATIC_LIBGCC := 1
    endif
  endif

  RT_LDFLAGS +=
endif

RT_LDFLAGS += $(RT_LIBS)

ifeq ($(STATICFORCE),1)
  # TODO(OSX)
  ifeq ($(OS),Linux)
    RT_LDFLAGS += -Wl,-Bdynamic
  endif
endif

ifeq ($(BUILD_PORTABLE),1)
  ifeq ($(OS),Linux)
    RT_LDFLAGS += -lgcc_s
    RT_LDFLAGS += -lgcc
  endif
endif

RT_LDFLAGS += $(LIB_SEARCH_PATHS)

RT_CXXFLAGS += -I$(TOP)/src
RT_CXXFLAGS += -pthread
RT_CXXFLAGS += "-DPRODUCT_NAME=\"$(PRODUCT_NAME)\""
RT_CXXFLAGS += "-D__STDC_LIMIT_MACROS"
RT_CXXFLAGS += "-D__STDC_FORMAT_MACROS"
RT_CXXFLAGS += -Wall -Wextra

RT_CXXFLAGS += -DENABLE_TLS

# Enable RapidJSON std::string functions
RT_CXXFLAGS += "-DRAPIDJSON_HAS_STDSTRING"
# Set RapidJSON to exact double parsing mode
RT_CXXFLAGS += "-DRAPIDJSON_PARSE_DEFAULT_FLAGS=kParseFullPrecisionFlag"

# Force 64-bit off_t size on Linux -- also, sizeof(off_t) will be
# checked by a compile-time assertion.
ifeq ($(OS),Linux)
  RT_CXXFLAGS += -D_FILE_OFFSET_BITS=64
endif



RT_CXXFLAGS += -Wnon-virtual-dtor -Wno-deprecated-declarations -std=gnu++0x

ifeq ($(COMPILER), INTEL)
  RT_CXXFLAGS += -w1 -ftls-model=local-dynamic

else ifeq ($(COMPILER), CLANG)
  RT_CXXFLAGS += -Wformat=2 -Wswitch-enum -Wswitch-default # -Wno-unneeded-internal-declaration
  RT_CXXFLAGS += -Wused-but-marked-unused -Wundef -Wvla -Wshadow
  RT_CXXFLAGS += -Wconditional-uninitialized -Wmissing-noreturn
else ifeq ($(COMPILER), GCC)
  ifeq ($(LEGACY_GCC), 1)
    RT_CXXFLAGS += -Wformat=2 -Wswitch-enum -Wswitch-default
  else
    RT_CXXFLAGS += -Wformat=2 -Wswitch-enum -Wswitch-default -Wno-array-bounds -Wno-maybe-uninitialized
  endif
endif

ifeq ($(COVERAGE), 1)
  ifeq ($(COMPILER), GCC)
    RT_CXXFLAGS += --coverage
    RT_LDFLAGS += --coverage
  else
    $(error COVERAGE=1 not yet supported for $(COMPILER))
  endif
endif

# TODO: >() only works on bash >= 4
LD_OUTPUT_FILTER ?=
ifeq ($(COMPILER),INTEL)
  # TODO: get rid of the cause of this warning, not just the warning itself
  LD_OUTPUT_FILTER += 2> >(grep -v "warning: relocation refers to discarded section")
endif

ifeq ($(RQL_ERROR_BT),1)
  RT_CXXFLAGS+=-DRQL_ERROR_BT
endif

# Configure debug vs. release
ifeq ($(DEBUG),1)
  RT_CXXFLAGS += -O0
else # ifeq ($(DEBUG),1)
  # use -fno-strict-aliasing to not break things
  # march=native used to break the serializer
  RT_CXXFLAGS += -O3 -DNDEBUG -fno-strict-aliasing # -march=native
endif # ifeq ($(DEBUG),1)

ifeq (${STATIC_LIBGCC},1)
  RT_LDFLAGS += -static-libgcc -static-libstdc++
endif

ifeq ($(SYMBOLS),1)
  # -rdynamic is necessary so that backtrace_symbols() works properly
  ifeq ($(OS),Linux)
    RT_LDFLAGS += -rdynamic
  endif
  RT_CXXFLAGS += -g
endif  # ($(SYMBOLS),1)

ifeq ($(LEGACY_LINUX),1)
  RT_CXXFLAGS += -DLEGACY_LINUX -DNO_EPOLL -Wno-format
endif

ifeq ($(LEGACY_GCC),1)
  RT_CXXFLAGS += -Wno-switch-default -Wno-switch-enum
endif

ifeq ($(NO_EVENTFD),1)
  RT_CXXFLAGS += -DNO_EVENTFD
endif

ifeq ($(NO_EPOLL),1)
  RT_CXXFLAGS += -DNO_EPOLL
endif

ifeq ($(THREADED_COROUTINES),1)
  RT_CXXFLAGS += -DTHREADED_COROUTINES
endif

ifeq ($(VALGRIND),1)
  ifneq (system,$(ALLOCATOR))
    $(error cannot build with VALGRIND=1 when using a custom allocator)
  endif
  RT_CXXFLAGS += -DVALGRIND
endif

ifeq ($(V8_PRE_3_19),1)
  RT_CXXFLAGS += -DV8_PRE_3_19
endif

ifeq ($(FULL_PERFMON),1)
  RT_CXXFLAGS += -DFULL_PERFMON
endif

ifeq ($(CORO_PROFILING),1)
  RT_CXXFLAGS += -DENABLE_CORO_PROFILER
endif

ifeq ($(HAS_TERMCAP),1)
  RT_CXXFLAGS += -DHAS_TERMCAP
endif

#### Finding what to build

SOURCES := $(shell find $(TOP)/src -name '*.cc' -not -name '\.*')

SERVER_EXEC_SOURCES := $(filter-out $(TOP)/src/unittest/%,$(SOURCES))

ifeq (/,$(firstword $(subst /,/ ,$(CWD))))
  DEPS_POSTFIX := .abs
else ifeq (.,$(CWD))
  DEPS_POSTFIX :=
else
  DEPS_POSTFIX := .$(subst /,_,$(subst ../,,$(CWD)))
endif

NAMES := $(patsubst $(TOP)/src/%.cc,%,$(SOURCES))
DEPS := $(patsubst %,$(DEP_DIR)/%$(DEPS_POSTFIX).d,$(NAMES))
OBJS := $(patsubst %,$(OBJ_DIR)/%.o,$(NAMES))

SERVER_EXEC_OBJS := $(patsubst $(TOP)/src/%.cc,$(OBJ_DIR)/%.o,$(SERVER_EXEC_SOURCES))

SERVER_NOMAIN_OBJS := $(patsubst $(TOP)/src/%.cc,$(OBJ_DIR)/%.o,$(filter-out %/main.cc,$(SOURCES)))

SERVER_UNIT_TEST_OBJS := $(SERVER_NOMAIN_OBJS) $(OBJ_DIR)/unittest/main.o

##### Version number handling

RT_CXXFLAGS += -DRETHINKDB_VERSION=\"$(RETHINKDB_VERSION)\"
RT_CXXFLAGS += -DRETHINKDB_CODE_VERSION=\"$(RETHINKDB_CODE_VERSION)\"

##### Build targets

ALL += $(TOP)/src
.PHONY: $(TOP)/src/all
$(TOP)/src/all: $(BUILD_DIR)/$(SERVER_EXEC_NAME) $(BUILD_DIR)/$(GDB_FUNCTIONS_NAME) | $(BUILD_DIR)/.

ifeq ($(UNIT_TESTS),1)
  $(TOP)/src/all: $(BUILD_DIR)/$(SERVER_UNIT_TEST_NAME)
endif

$(TOP)/src/rpc/semilattice/joins/macros.hpp: $(TOP)/scripts/generate_join_macros.py
$(TOP)/src/rpc/serialize_macros.hpp: $(TOP)/scripts/generate_serialize_macros.py
$(TOP)/src/rpc/semilattice/joins/macros.hpp $(TOP)/src/rpc/serialize_macros.hpp:
	$P GEN $@
	$< > $@

.PHONY: generate-headers
generate-headers: $(TOP)/src/rpc/semilattice/joins/macros.hpp $(TOP)/src/rpc/serialize_macros.hpp

$(TOP)/src/rdb_protocol/ql2.pb.h $(TOP)/src/rdb_protocol/ql2.pb.cc: $(TOP)/src/rdb_protocol/ql2.proto
	$P PROTOC
#	# See issue #2965
	+rm -f $(TOP)/src/rdb_protocol/ql2.pb.h $(TOP)/src/rdb_protocol/ql2.pb.cc
	$(PROTOC) --proto_path=$(TOP)/src --cpp_out $(TOP)/src $<

.PHONY: generate-protos
generate-protos: $(TOP)/src/rdb_protocol/ql2.pb.h $(TOP)/src/rdb_protocol/ql2.pb.cc

.PHONY: rethinkdb
rethinkdb: $(BUILD_DIR)/$(SERVER_EXEC_NAME)

RETHINKDB_DEPENDENCIES_LIBS := $(MALLOC_LIBS_DEP) $(V8_LIBS_DEP) $(PROTOBUF_LIBS_DEP) $(RE2_LIBS_DEP) $(Z_LIBS_DEP) $(CURL_LIBS_DEP) $(CRYPTO_LIBS_DEP) $(SSL_LIBS_DEP)

MAYBE_CHECK_STATIC_MALLOC =
ifeq ($(STATIC_MALLOC),1) # if the allocator is statically linked
  ifeq (tcmalloc,$(ALLOCATOR))
    MAYBE_CHECK_STATIC_MALLOC = objdump -T $@ | c++filt | grep -q 'tcmalloc::\|google_malloc' ||
    MAYBE_CHECK_STATIC_MALLOC += (echo "Failed to link in TCMalloc." >&2 && false)
  else ifeq (jemalloc,$(ALLOCATOR))
    RT_LDFLAGS += -ldl
    MAYBE_CHECK_STATIC_MALLOC = objdump -T $@ | grep -w -q 'mallctlnametomib' ||
    MAYBE_CHECK_STATIC_MALLOC += (echo "Failed to link in jemalloc." >&2 && false)
  endif
endif

ifneq (1,$(SYMBOLS))
  ifeq (1,$(SPLIT_SYMBOLS))
    $(error Conflicting build flags: SYMBOLS=0 and SPLIT_SYMBOLS=1)
  endif
endif

$(BUILD_DIR)/$(SERVER_EXEC_NAME): $(SERVER_EXEC_OBJS) | $(BUILD_DIR)/. $(RETHINKDB_DEPENDENCIES_LIBS)
	$P LD $@
	$(RT_CXX) $(SERVER_EXEC_OBJS) $(RT_LDFLAGS) -o $(BUILD_DIR)/$(SERVER_EXEC_NAME) $(LD_OUTPUT_FILTER)
	$(MAYBE_CHECK_STATIC_MALLOC)

ifeq (1,$(SPLIT_SYMBOLS))
  ifeq (Darwin,$(OS))
	$P STRIP $@.dSYM
	cd $(BUILD_DIR) && dsymutil --out=$(notdir $@.dSYM) $(notdir $@)
	strip $@
  else
	$P STRIP $@.debug
	objcopy --only-keep-debug $@ $@.debug
	objcopy --strip-debug $@
	cd $(BUILD_DIR) && objcopy --add-gnu-debuglink=$(notdir $@.debug) $(notdir $@)
  endif
endif

# The unittests use gtest, which uses macros that expand into switch statements which don't contain
# default cases. So we have to remove the -Wswitch-default argument for them.
$(SERVER_UNIT_TEST_OBJS): RT_CXXFLAGS := $(filter-out -Wswitch-default,$(RT_CXXFLAGS)) $(GTEST_INCLUDE)

$(SERVER_UNIT_TEST_OBJS): | $(GTEST_INCLUDE_DEP)

$(BUILD_DIR)/$(SERVER_UNIT_TEST_NAME): $(SERVER_UNIT_TEST_OBJS) $(GTEST_LIBS_DEP) | $(BUILD_DIR)/. $(RETHINKDB_DEPENDENCIES_LIBS)
	$P LD $@
	$(RT_CXX) $(SERVER_UNIT_TEST_OBJS) $(RT_LDFLAGS) $(GTEST_LIBS) -o $@ $(LD_OUTPUT_FILTER)

$(BUILD_DIR)/$(GDB_FUNCTIONS_NAME): | $(BUILD_DIR)/.
	$P CP $@
	cp $(TOP)/scripts/$(GDB_FUNCTIONS_NAME) $@

$(OBJ_DIR)/%.o: $(TOP)/src/%.cc $(MAKEFILE_DEPENDENCY) $(ALL_INCLUDE_DEPS)
	mkdir -p $(dir $@) $(dir $(DEP_DIR)/$*)
	$P CC
	$(RT_CXX) $(RT_CXXFLAGS) -c -o $@ $< \
	          -MP -MQ $@ -MD -MF $(DEP_DIR)/$*$(DEPS_POSTFIX).d
	test $(DEP_DIR)/$*$(DEPS_POSTFIX).d -nt $< || ( \
	  echo 'Warning: Missing dep file: `$(DEP_DIR)/$*$(DEPS_POSTFIX).d` should have been generated by $(RT_CXX)' ; \
	  sleep 1; touch $< \
	)

FORCE_ALL_DEPS := $(patsubst %,force-dep/%,$(NAMES))
force-dep/%: $(TOP)/src/%.cc $(ALL_INCLUDE_DEPS)
	$P CXX_DEPS $(DEP_DIR)/$*$(DEPS_POSTFIX).d
	mkdir -p $(dir $(DEP_DIR)/$*)
	$(RT_CXX) $(RT_CXXFLAGS) $(TOP)/src/$*.cc -MP -MQ $(OBJ_DIR)/$*.o -M -MF $(DEP_DIR)/$*$(DEPS_POSTFIX).d

.PHONY: deps
deps: $(FORCE_ALL_DEPS)

-include $(DEPS)

.PHONY: build-clean
build-clean:
	$P RM $(BUILD_ROOT_DIR)
	rm -rf $(BUILD_ROOT_DIR)

# For emacs' flymake-mode
.PHONY: check-syntax
check-syntax:
	$(RT_CXX) $(RT_CXXFLAGS) -c -o /dev/null $(patsubst %,$(CWD)/%,$(CHK_SOURCES))
