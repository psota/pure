# Author: James Psota
# File:   Makefile.include.mk 

# Copyright (c) 2024 James Psota
# __________________________________________

# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
# 
#   http://www.apache.org/licenses/LICENSE-2.0
# 
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

#$(warning Makefile include start $(shell date -I"ns"))

# Paths are set up such that this file should be included from one directory below this one.
export SHELL = /bin/bash

# export all the variables that the pure runtime system also needs access to
export RUNTIME = Pure
export DEBUG ?= 1
export PAUSE_FOR_DEBUGGER_ATTACH ?= 0
export PROFILE ?= 0
export PURE_NUM_PROCS
export NPROCS = $(PURE_NUM_PROCS)
export PURE_RT_NUM_THREADS
export THREADS_PER_NODE_LIMIT
export RUN_THREAD_DEBUGGER
export TOTAL_MPI_THREADS
export TRACE_COMM ?= 0
export TSAN ?= 0
export ASAN ?= 0
export MSAN ?= 0
export UBSAN ?= 0
export ENABLE_THREAD_LOGGER ?= 0
export NUMA_SCHEME ?= none
export PROCESS_CHANNEL_VERSION
export PROCESS_CHANNEL_MAJOR_VERSION := $(shell echo "$(PROCESS_CHANNEL_VERSION)" | cut -c1)
export PCV_4_NUM_CONT_CHUNKS
export DO_PRINT_CONT_DEBUG_INFO ?= 0

# warning! we ran most results with 8192 but thesis results show that 4096 is optimal for most node pairs. Be aware of this.
export BUFFERED_CHAN_MAX_PAYLOAD_BYTES ?= 4096
export PROCESS_CHANNEL_BUFFERED_MSG_SIZE
export PRINT_PROCESS_CHANNEL_STATS
export RUN_ARGS
export TRACE_MPI_CALLS ?= 0
export NERSC_BATCH_MODE ?= 0
export ENABLE_THREAD_AFFINITY_AUTOMAPPER ?= 0
export TOTAL_THREADS ?= $(shell echo "$(NPROCS) * $(PURE_RT_NUM_THREADS)" | bc)
export MPI_PROCS_PER_NODE_LIMIT ?= 1  # one MPI process per node for Pure unless overridden
export NO_SLURM_FILE_LOG
export CHECK_PADDING ?= 0
export VALGRIND_MODE ?= 0
export COLLECT_THREAD_TIMELINE_DETAIL ?= 0
export JSON_STATS_DIR ?= runs/temp_latest
export USE_JEMALLOC ?= 0
export DO_JEMALLOC_PROFILE ?= 0
export MALLOC_CONF 
export USE_ASMLIB ?= 0
export DISABLE_PURIFICATION ?= 0
export USE_TRACEANALYZER
export SHOW_SKIPPED_INTERVALS ?= 0
export TIMER_INTERVAL_NUM_GUESS
# don't include intervals that end after the end-to-end timer starts or start after the end-to-end timer ends
export TRUNCATE_INTERVALS_OUTSIDE_ETE ?= 1
export PURE_APP_TIMER ?= 0
export INITIAL_OUTSTANDING_CHANNEL_DEQ_REQ_CLS ?= 1
export MAP_PROFILE ?= 0
export DO_PURE_EXHAUST_REMAINING_WORK ?= 1
export USE_HELPER_THREADS_IF_FREE_CORES ?= 0
export PIN_HELPER_THREADS_IF_NUMA_SCHEME ?= 1
export PURE_MAX_HELPER_THREADS ?=
export PCV4_OPTION_DIRECT_CHANNEL_WORK_STEAL ?= 0
export PCV4_OPTION_DIRECT_CHANNEL_WORK_STEAL_TRIES ?= 4000
export PCV4_OPTION_WORK_STEAL_EXHAUSTIVELY ?= 0
export PURE_PIPELINE_DIRECT_STEAL ?= 0
export PURE_DMC_TEST_WAIT ?= 0
export PURE_DMC_TEST_WAIT_DELAY_USECS ?= 0
export FORCE_MPI_THREAD_SINGLE ?= 0
export DO_LIST_WAITALL ?= 1

export ENABLE_PURE_FLAT_BATCHER ?= 0
export NUM_DIRECT_CHAN_BATCHERS_PER_MATE ?= 4
export BATCHER_DEBUG_PRINT ?= 0
export MAX_DIRECT_CHAN_BATCHER_MESSAGE_PAYLOAD_BYTES ?= 16384
export DCB_MAX_MESSAGES_PER_BATCH ?= 16
export DCB_SEND_BYTES_THRESHOLD ?= -1

# must define!
export MAX_BYTES_FOR_INTRANODE_QUEUE_STYLE

export MPI_HOSTFILE
export ENABLE_MPI_ERROR_RETURN ?= 0
export USE_RDMA_MPI_CHANNEL ?= 0 # LATER -- change default?
export OVERRIDE_RUNTIME
export OBJ_DIR_DEBUG_VERBOSE ?= 0
export THREAD_MAP_FILENAME
#########################################
export LINK_TYPE ?= dynamic  # may want to change this default
export CORI_ARCH

# for cori force to turn off DMAPP.
export USE_MPI_DMAPP = 0
export LARGE_MPI_TAG ?= 0

#define PURE_RDMA_MODE_FENCE 0 -- not currently supported
#define PURE_RDMA_MODE_PSCW 1
#define PURE_RDMA_MODE_LOCK 2
# note: all of these are currenty broken.
export PURE_RDMA_MODE ?= 1  # 0 is fence, 1 is PSCW, 2 is locks

############### Batcher stuff
export FLAT_BATCHER_RECV_VERSION ?= 2
export FLAT_BATCHER_SEND_BYTES_THRESHOLD ?= 0
export SENDER_BATCHER_DEBUG_PRINT ?= 0
export RECEIVER_BATCHER_DEBUG_PRINT ?= 0
export PURE_RANK_0_STATS_ONLY ?= 0

export NERSC_COMPILER_OVERRIDE ?= 
export ENABLE_HYPERTHREADS
#############################################################################

ifeq ($(PURE_NUM_PROCS),)
    $(error Environment variable PURE_NUM_PROCS must be set and be an integer or AUTO)
endif
ifeq ($(PURE_RT_NUM_THREADS),)
    $(error Environment variable PURE_RT_NUM_THREADS must be set and be an integer or AUTO)
endif

ifeq ($(APP_DEBUG),1)
	CXXFLAGS += -DAPP_DEBUG
endif
ifeq ($(APP_DEBUG_VERBOSE),1)
	CXXFLAGS += -DAPP_DEBUG_VERBOSE
endif
ifeq ($(COLLECT_STATS),1)
	CXXFLAGS += -DCOLLECT_STATS=1
endif
ifeq ($(ALLOW_INVALID_RESULT),1)
	CXXFLAGS += -DALLOW_INVALID_RESULT=1
endif

SUPPORT_PATH = $(CPL)/support

# we add the link type to change the OBJ_DIR path
COMMON_C_FLAGS += -Wall 
CFLAGS += $(COMMON_C_FLAGS) $(USER_CFLAGS)
CXXFLAGS += $(COMMON_C_FLAGS) $(USER_CXXFLAGS) $(USER_CPPFLAGS)


# TODO: clean this up. probably including too much stuff here.
INCLUDES += -I$(MAKEFILE_DIR)../include/ $(USER_INCLUDES)
MAKEDEPEND_INCLUDES += $(INCLUDES)
LFLAGS += $(USER_LFLAGS)
LIBS += -lpthread $(USER_LIBS)

# override on CRAY (Cori)
ifeq ($(OS), nersc)
	LINK_TYPE = $(CRAYPE_LINK_TYPE)
endif

#$(warning >>> LINKING USING LINK TYPE: $(LINK_TYPE) <<<)

# libpure configurations

#$(warning INCLUDING START $(shell date -I"ns"))

LIBS += -lpure
#$(warning INCLUDING START 1 $(shell date -I"ns"))
include $(SUPPORT_PATH)/Makefile_includes/Makefile.build_setup.mk
#$(warning INCLUDING START 2 $(shell date -I"ns"))
include $(SUPPORT_PATH)/Makefile_includes/Makefile.libpure_path.mk
LIBPURE_PATH_FLAGS = -L$(LIB_PREFIX)
ifeq ($(LINK_TYPE), dynamic)
	# add this on
	LIBPURE_PATH_FLAGS += -Wl,-rpath,$(LIB_PREFIX)
endif


# current version of clang doesn't support restrict keyword. eliding it for osx for now. 
# TODO: update clang on laptops.
ifeq ($(OS),osx)
	CFLAGS   += -x c
	INCLUDES += -I/Users/jim/local/include/
endif

#$(warning INCLUDING START 3 $(shell date -I"ns"))
include $(SUPPORT_PATH)/Makefile_includes/Makefile.sanitizer_opts.mk

#############################################################################
# all cflags and cxxflags for the program must be defined above this point
export VERBOSE_OBJ_DIR ?= 1
#$(warning INCLUDING START  4 $(shell date -I"ns"))
include $(SUPPORT_PATH)/Makefile_includes/Makefile.misc.mk
#$(warning INCLUDING START 5 $(shell date -I"ns"))
include $(SUPPORT_PATH)/Makefile_includes/Makefile.application.mk

#$(warning INCLUDING END $(shell date -I"ns"))

ifeq ($(ENABLE_THREAD_LOGGER), 1)
	PURIFIED_SRCS_NAME_SUFFIX = logged
else
	PURIFIED_SRCS_NAME_SUFFIX = not-logged
endif
export PURIFIED_SRCS_NAME_SUFFIX

USER_C_SRCS   = $(filter %.c, $(PURE_USER_CODE_SRCS))
USER_CPP_SRCS = $(filter %.cpp, $(PURE_USER_CODE_SRCS))
PURIFIED_USER_CPP_SRCS = $(patsubst %.cpp, %.$(PURIFIED_SRCS_NAME_SUFFIX).purified.cpp, $(USER_CPP_SRCS))

# note: I think this doesn't make sense because Pure only works on cpp sources.
# PURIFIED_USER_C_SRCS   = $(patsubst %.c, %.$(PURIFIED_SRCS_NAME_SUFFIX).purified.c, $(USER_C_SRCS))
#PURIFIED_USER_CODE_SRCS = $(PURIFIED_USER_CPP_SRCS) $(PURIFIED_USER_C_SRCS)
PURIFIED_USER_CODE_SRCS = $(PURIFIED_USER_CPP_SRCS)
ALL_PURIFIED_USER_CODE_SRCS = $(wildcard *.purified.c*)

# TESTING TODO
#OTHER_SRCS = $(MAKEFILE_DIR)../src/3rd_party/jsoncpp/dist/jsoncpp.cpp

LIBPURE_MAKEFILE_PATH = $(MAKEFILE_DIR)../src

# LIB_DIR = $(MAKEFILE_DIR)../lib
# LIB_PREFIX := $(LIB_DIR)/$(OBJ_DIR_PREFIX)
# LIBPURE_FULL_PATH = $(LIB_PREFIX)/libpure.a


PURIFIED_OBJS += $(patsubst %.purified.cpp, $(BUILD_TEST_DIR)/%.purified.cpp.o, $(PURIFIED_USER_CPP_SRCS))

C_OBJS = $(patsubst %.c, $(BUILD_TEST_DIR)/%.c.o, $(USER_C_SRCS))
CPP_OBJS = $(patsubst %.cpp, $(BUILD_TEST_DIR)/%.cpp.o, $(USER_CPP_SRCS))

# Configure object files depending on purification or not (manual purification)
ifeq ($(DISABLE_PURIFICATION),1)
	# take CPP files directly -- don't purify
	ALL_OBJS = $(C_OBJS) $(CPP_OBJS) 
else
	ALL_OBJS = $(C_OBJS) $(PURIFIED_OBJS)
endif	


#PURIFIED_OBJS += $(patsubst %.purified.c, $(BUILD_TEST_DIR)/%.purified.c.o, $(PURIFIED_USER_C_SRCS))
#OTHER_OBJS    := $(patsubst %.cpp, $(BUILD_PREFIX)/%.cpp.o, $(OTHER_SRCS))

ifeq ($(ENABLE_MPI_DEBUG),1)
	export MPICH_DBG_FILENAME = mpi_debug_logs/dbg-%w-%d.log
# for some reason this doesn't work
#	export MPICH_DBG_CLASS = ALL    
	export MPICH_DBG_LEVEL = VERBOSE
	export MPICH_DBG = YES
else
	unexport MPICH_DBG_FILENAME
	unexport MPICH_DBG_LEVEL
	unexport MPICH_DBG
endif


# Automatic dependency generation
# autodep article: http://make.mad-scientist.net/papers/advanced-auto-dependency-generation/#tldr
# the dummy directories are to allow some safety from name collisions for dependency dirs 
export DEPDIR := $(BUILD_TEST_DIR)/.d/dummy1/dummy2
EXTRA_USER_DEPFILES += $(CPL)/include/pure/common/pure.h

# TODO: keep this updated if you add other src subdirs
$(shell mkdir -p $(DEPDIR)   >/dev/null)
DEPFLAGS = -MT $@ -MMD -MP -MF $(DEPDIR)/$*.Td
#DEP_POSTCOMPILE = if [ -f $(DEPDIR)/$*.Td ]; then mv -f $(DEPDIR)/$*.Td $(DEPDIR)/$*.d && touch $@; fi
DEP_POSTCOMPILE = mv -f $(DEPDIR)/$*.Td $(DEPDIR)/$*.d || echo "Already moved it" && touch $@

#########################################################################################

.PRECIOUS: $(PURIFIED_USER_CODE_SRCS)
.PHONY: clean clean_test app_prebuild libpure run debug gdb gdb-setup gdb-run valgrind valkyrie helgrind purify-all \
	    analyze check-valgrind-thread-config vars clean_stats process-mpi-logs nothing setup remove_ansi_codes \
	    valgrind-cleanup clean_all cat_build_opts verify_valgrind_opts massif ms_print \
	    profile profile-int verify_profile_opts mpip flamegraph clean_profile mpip mpiP profile-int-no-profile \
	    profile-stat all_default application color_test
.DEFAULT: all_default
.DEFAULT_GOAL: all_default

#$(warning LOADED Makefile.include. about to start target $@...  $(shell date -I"ns"))

all_default: main-prebuild
	@echo
	@echo "DONE running target main-prebuild."
	@echo
	@echo -e "$(KLTBLUE)__ Compilation of $(MAIN) complete.$(KRESET)"
	@echo -e "$(KLTCYAN)   [branch: $(shell git rev-parse --abbrev-ref HEAD) $(shell git rev-parse --short HEAD)   CORI_ARCH: $(CORI_ARCH)]        ___________________________________________$(KRESET)"
	@echo

main-prebuild: app_prebuild libpure purify-all .verify_obj_dir .verify_pcv_vars
	$(MAKE) -e $(MAIN)

libpure:
	OBJ_DIR_PREFIX=$(OBJ_DIR_PREFIX) LINK_TYPE=$(LINK_TYPE) OVERRIDE_RUNTIME=$(OVERRIDE_RUNTIME) $(MAKE) -C $(LIBPURE_MAKEFILE_PATH)

libpure-vars:
	OBJ_DIR_PREFIX=$(OBJ_DIR_PREFIX) LINK_TYPE=$(LINK_TYPE) OVERRIDE_RUNTIME=$(OVERRIDE_RUNTIME) $(MAKE) -C $(LIBPURE_MAKEFILE_PATH) vars

# this SHOULD NOT be called directly. It's essential that this is executed via $(MAIN)
$(MAIN): $(LIBPURE_FULL_PATH) $(ALL_OBJS) $(EXTRA_USER_DEPFILES)
	@echo -e "$(KCYAN) ☵☵☵☵☵☵☵☵☵☵   rebuilding APPLICATION $(MAIN)   ☵☵☵☵☵☵☵☵☵☵$(KRESET)"
	$(SUPPORT_PATH)/misc/rename_depfiles.rb $(DEPDIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $(MAIN) $(LFLAGS) $(ALL_OBJS)  $(LIBPURE_PATH_FLAGS) $(LIBS)

process-mpi-logs:
ifeq ($(ENABLE_MPI_DEBUG),1)
	$(SUPPORT_PATH)/misc/process_mpi_logs.rb $(TEST_DIR)
else
	@echo "MPI Debugging logs not enabled. Enable in application Makefile (ENABLE_MPI_DEBUG)"
endif

run: main-prebuild  numactl_status
ifeq ($(ASAN),1)
	$(RM_F) $(SANITIZER_LOG_DIR)/$(ASAN_OUT_LOGFILE)*

ifeq ($(OS),nersc)
	@echo "Forcing dynamic libs for ASAN on NERSC -- doesn't seem to work. Right now run this manually."
	$(shell module unload static_libs)
endif
endif
ifeq ($(TSAN),1)
	$(RM_F) $(SANITIZER_LOG_DIR)/$(TSAN_OUT_LOGFILE)*
endif
ifeq ($(MSAN),1)
	$(RM_F) $(SANITIZER_LOG_DIR)/$(MSAN_OUT_LOGFILE)*
endif
ifeq ($(UBSAN),1)
	$(RM_F) $(SANITIZER_LOG_DIR)/$(UBSAN_OUT_LOGFILE)*
endif
ifeq ($(COLLECT_THREAD_TIMELINE_DETAIL),1)
	$(RM_F) timer_interval/*
endif
ifeq ($(TRACE_COMM),1)
	$(RM_F) runs/temp_latest/pure_comm_trace_r*.csv
endif
ifeq ($(PRINT_PROCESS_CHANNEL_STATS),1)
	$(RM_F) runs/temp_latest/wait_stats_process_*.csv
endif
	$(RM_RF) $(JSON_STATS_DIR)/*

	@echo "████████████████████████████████████████████████████████████████████████████████████"
	JSON_STATS_DIR=$(JSON_STATS_DIR) $(RUN_CMD) $(INTERACTIVE_SUFFIX)
	@JSON_STATS_DIR=$(JSON_STATS_DIR) make app_postsuccess

	$(shell [ -d $(RUN_OUTPUT_LOG_DIR) ] || mkdir $(RUN_OUTPUT_LOG_DIR))
	$(MAKE) FILE_TO_REMOVE_ANSI_FROM=$(RUN_OUTPUT_FILENAME) remove_ansi_codes
	$(MAKE) process-mpi-logs
ifeq ($(ASAN),1)
ifeq ($(ASAN_COMMIT_LOGS),1)
	@echo "Number of sanitizer out files in directory $(SANITIZER_LOG_DIR):"
	[[ -e $(SANITIZER_LOG_DIR)/$(ASAN_OUT_LOGFILE)* ]] && ls -l $(SANITIZER_LOG_DIR)/$(ASAN_OUT_LOGFILE)* | wc -l
	git add $(SANITIZER_LOG_DIR)/$(ASAN_OUT_LOGFILE)*
	git commit -m "Adding new ASAN output logs" $(SANITIZER_LOG_DIR)/$(ASAN_OUT_LOGFILE)*
	$(SUPPORT_PATH)/misc/asan_totals.rb $(SANITIZER_LOG_DIR)/$(ASAN_OUT_LOGFILE)*
	echo "Added ASAN logs; still have to git push"
endif
endif
ifeq ($(ENABLE_THREAD_AFFINITY_AUTOMAPPER),1)
	@echo ENABLE_THREAD_AFFINITY_AUTOMAPPER set to 1. Therefore, using threadmap called $(THREAD_AFFINITY_AUTOMAPPER_FILENAME) with $(PURE_NUM_PROCS) procs and $(PURE_RT_NUM_THREADS) threads
endif
ifeq ($(ASAN),1)
ifeq ($(OS),nersc)
	@echo Resetting to static libs state again
	$(shell module load static_libs)
endif

endif
	@echo "See $(RUN_OUTPUT_FILENAME) for the output from this run."


include $(SUPPORT_PATH)/Makefile_includes/Makefile.profiling.mk

mpip: LIBS += -lmpiP -lm -lbfd -liberty -lunwind
mpip: verify_profile_opts run
	cp $(MPIP_LOG_DIR)/$(shell ls -t $(MPIP_LOG_DIR) | head -1) $(TEMP_WEB_DIR)

mpiP: mpip

EXTRA_DEBUGINFO_PATH ?= $(LIB_PREFIX)
include $(SUPPORT_PATH)/Makefile_includes/Makefile.valgrind_targets.mk

purify-all: $(USER_CPP_SRCS)
ifeq ($(DISABLE_PURIFICATION),0)
	@echo "Purifying ALL user CPP sources via direct invocation."
	$(SUPPORT_PATH)/runtime/purify_all.rb $^
endif

check-valgrind-thread-config-pure-lib:
	@echo "BEFORE check-valgrind-thread-config-pure-lib:"
	$(MAKE) -j1 -k -C $(LIBPURE_MAKEFILE_PATH) check-valgrind-thread-config
	@echo "AFTER check-valgrind-thread-config-pure-lib:"

check-valgrind-thread-config: check-valgrind-thread-config-pure-lib
	$(warn Checking that source files are configured properly.)
	$(SUPPORT_PATH)/misc/check_valgrind_thread_config.rb $(PURIFIED_USER_CODE_SRCS)

# These object files are placed into a directory under the build directory that mirrors the test directory.
# for example: build/debug/ASAN/no_thread_logging/test/c_test/c_test.purified.c.o
$(BUILD_TEST_DIR)/%.c.o: %.c $(DEPDIR)/%.d $(EXTRA_USER_DEPFILES)
	$(shell [ -d $(shell dirname $@) ] || mkdir -p $(shell dirname $@) )
	$(CC) $(DEPFLAGS) $(CFLAGS) $(INCLUDES) -c $< -o $@
	$(DEP_POSTCOMPILE)

$(BUILD_TEST_DIR)/%.cpp.o: %.cpp $(DEPDIR)/%.d $(EXTRA_USER_DEPFILES)
	$(shell [ -d $(shell dirname $@) ] || mkdir -p $(shell dirname $@) )
	$(CXX) $(DEPFLAGS) $(CXXFLAGS) $(INCLUDES) -c $< -o $@
	$(DEP_POSTCOMPILE)

vars:
	@echo "                     DEBUG: $(DEBUG)"
	@echo "                   PROFILE: $(PROFILE)"
	@echo "                  BIN_NAME: $(BIN_NAME)"
	@echo "             MAKEFILE_PATH: $(MAKEFILE_PATH)"
	@echo "              MAKEFILE_DIR: $(MAKEFILE_DIR)"
	@echo "                  TEST_DIR: $(TEST_DIR)"
	@echo "       PURE_USER_CODE_SRCS: $(PURE_USER_CODE_SRCS)"
	@echo "    PURIFIED_USER_CPP_SRCS: $(PURIFIED_USER_CPP_SRCS)"
	@echo "      PURIFIED_USER_C_SRCS: $(PURIFIED_USER_C_SRCS)"
	@echo "ALL_PURIFIED_USER_CODE_SRCS: $(ALL_PURIFIED_USER_CODE_SRCS)"
	@echo "             PURIFIED_OBJS: $(PURIFIED_OBJS)"
	@echo "            OBJ_DIR_PREFIX: $(OBJ_DIR_PREFIX)"
	@echo "                  ALL_OBJS: $(ALL_OBJS)"
	@echo "               USER_C_SRCS: $(USER_C_SRCS)"
	@echo "                    DEPDIR: $(DEPDIR)"		
	@echo "                  INCLUDES: $(INCLUDES)"
	@echo "                        CC: $(CC)"
	@echo "                       CXX: $(CXX)"
	@echo "                    CFLAGS: $(CFLAGS)"
	@echo "                  CXXFLAGS: $(CXXFLAGS)"
	@echo "                    LFLAGS: $(LFLAGS)"
	@echo "                      LIBS: $(LIBS)"
	@echo "                     NPROC: $(NPROC)"
	@echo "               NUMA_SCHEME: $(NUMA_SCHEME)"
	@echo "              NUMACTL_CPUS: $(NUMACTL_CPUS)"
	@echo "--------------------------------------------------------"
	@echo "     APP_PREBUILD_MAKEFILE: $(APP_PREBUILD_MAKEFILE)"
	@echo "      ENABLE_THREAD_LOGGER: $(ENABLE_THREAD_LOGGER)"
	@echo "              BUILD_PREFIX: $(BUILD_PREFIX)"
	@echo "            BUILD_TEST_DIR: $(BUILD_TEST_DIR)"
	@echo " PURIFIED_SRCS_NAME_SUFFIX: $(PURIFIED_SRCS_NAME_SUFFIX)"
	@echo "                   NUMACTL: $(NUMACTL)"
	@echo "              NUMACTL_CPUS: $(NUMACTL_CPUS)"
	@echo "              NUMACTL_DESC: $(NUMACTL_DESC)"
	@echo "              SUPPORT_PATH: $(SUPPORT_PATH)"
	@echo "              TSAN_OPTIONS: $(TSAN_OPTIONS)"
	@echo "         SANITIZER_LOG_DIR: $(SANITIZER_LOG_DIR)"
	@echo "                FLAGS_HASH: $(FLAGS_HASH)"
	@echo "          PURE_LIB_SOURCES: $(PURE_LIB_SOURCES)"
	@echo "                TRACE_COMM: $(TRACE_COMM)"
	@echo "               MPI_ENABLED: $(MPI_ENABLED)"
	@echo "--------------------------------------------------------"
	@echo "          OVERRIDE_RUNTIME: $(OVERRIDE_RUNTIME)"
	@echo "       THREAD_MAP_FILENAME: $(THREAD_MAP_FILENAME) "
	@echo "   PURE_MAX_HELPER_THREADS: $(PURE_MAX_HELPER_THREADS)"
	@echo "--------------------------------------------------------"
	@echo "                    NPROCS: $(NPROCS)"
	@echo "       PURE_RT_NUM_THREADS: $(PURE_RT_NUM_THREADS)"
	@echo "             TOTAL_THREADS: $(TOTAL_THREADS)"
	@echo "    THREADS_PER_NODE_LIMIT: $(THREADS_PER_NODE_LIMIT)"
	@echo "       ENABLE_HYPERTHREADS: $(ENABLE_HYPERTHREADS)"
	@echo "        SLURM_NODE_OPTIONS: $(SLURM_NODE_OPTIONS)"
	@echo "                  BIN_NAME: $(BIN_NAME)"
	@echo "                   RUN_CMD: $(RUN_CMD)"
	@echo


.PHONY: list-targets
list-targets:
	@$(MAKE) -pRrq -f /Users/jim/Documents/Research/projects/pure_trees/rt_buffer/hybrid_programming/pure/test/Makefile.include.mk : 2>/dev/null | awk -v RS= -F: '/^# File/,/^# Finished Make data base/ {if ($$1 !~ "^[#.]") {print $$1}}' | sort | egrep -v -e '^[^[:alnum:]]' -e '^$@$$' | xargs

export_env:
	#@echo "$(shell env)"
	@echo "$(.VARIABLES)"

# this is how you print color in Makefiles (assuming these variables are exported via bashrcs)
color_test:
	@echo -e "$(KRED)$@ is here$(KRESET)"

# See all warnings
analyze: CXXFLAGS += $(CXXFLAGS_FOR_ANALYZE)
analyze:
	@echo "Statically analyzing code with some checking options. See all warning options at https://gcc.gnu.org/onlinedocs/gcc/Warning-Options.html"
	$(MAKE) -k -C $(LIBPURE_MAKEFILE_PATH) analyze
	$(MAKE) clean
	make

# TODO: create --analyze option to run the clang analyzer
tidy:
	$(MAKE) -k -C $(LIBPURE_MAKEFILE_PATH) tidy
	clang-tidy -checks=* $(PURIFIED_USER_CODE_SRCS) -- $(CXXFLAGS) $(INCLUDES) > $(TIDY_LOG)
	@echo "See clang-tidy output in $(TIDY_LOG) in $(TEST_DIR)"

tidy-single:
	@echo "You must set variable TF with the file you want to tidy (TF = \"Tidy File\")"
	clang-tidy -checks=* $(TF) -- $(CXXFLAGS) $(INCLUDES)


# TODO: there's a bug here -- it doesn't seem to respect the DEBUG flag
clean: clean_test clean_profile
	OBJ_DIR_PREFIX=$(OBJ_DIR_PREFIX) $(MAKE) -k -C $(LIBPURE_MAKEFILE_PATH) clean

clean_build_prefix: clean 
ifndef BUILD_EXTRA_PREFIX
	$(error BUILD_EXTRA_PREFIX must be defined for target $@)
endif
	$(RM_RF) $(BUILD_DIR)/$(BUILD_EXTRA_PREFIX)/*
	$(RM_RF) $(LIB_DIR)/$(BUILD_EXTRA_PREFIX)/*
	$(RM_F) $(BIN_NAME)

clean_all: clean_test clean_profile clean_stats clean_timer_intervals
	OBJ_DIR_PREFIX=$(OBJ_DIR_PREFIX) make -k -C $(LIBPURE_MAKEFILE_PATH) clean_all
	$(RM_RF) $(BUILD_DIR)/*
	$(RM_RF) $(LIB_DIR)/*
	$(RM_F)  $(ALL_PURIFIED_USER_CODE_SRCS)
	$(RM_F) $(BIN_NAME)

clean_test:
	$(RM_F) $(PURIFIED_USER_CODE_SRCS)
	$(RM_F) $(MAIN) *.o *~ 
	$(RM_RF) $(MAIN).dSYM
	$(RM_F) $(ALL_OBJS)
	$(RM_F) $(SANITIZER_LOG_DIR)/*
	$(RM_F) $(VALGRIND_LOG_DIR)/*
	$(RM_F) $(MPIP_LOG_DIR)/*
	$(RM_F) $(GDB_SCRIPT)
ifneq ($(strip $(EXTRA_CLEAN_TEST_COMMANDS)),"")
	$(EXTRA_CLEAN_TEST_COMMANDS)
endif

clean_profile:
	$(RM_F) perf.data perf.data.old out.perf out.perf.folded 

clean_stats:
	$(RM_F) *.json

clean_run:
	$(MAKE) clean
	$(MAKE) run

clean_make:
	$(MAKE) clean
	$(MAKE)

clean_timer_intervals:
	$(RM_F) timer_interval/*

nothing:

include $(SUPPORT_PATH)/Makefile_includes/Makefile.extra_targets.mk

$(DEPDIR)/%.d: ;
.PRECIOUS: $(DEPDIR)/%.d

-include $(patsubst %,$(DEPDIR)/%.d,$(basename $(PURE_LIB_SOURCES)))
