# Author: James Psota
# File:   Makefile.nonpure.mk 

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

# basic definitions and targets for baseline (non-Pure) benchmarks (MPI, MPI+OMP, no runtime, etc.)

#export NPROCS ?= $(PURE_NUM_PROCS)
export GDB_COMMANDS
export NUMA_SCHEME ?= none
export RUN_ARGS
#export RUNTIME = MPI
export THREADS_PER_NODE_LIMIT ?= nil
export TRACE_MPI_CALLS ?= 0
export NERSC_BATCH_MODE ?= 0
export PURE_RT_NUM_THREADS ?= 1
export TOTAL_MPI_THREADS ?= $(NPROCS)
export MPI_PROCS_PER_NODE_LIMIT
export NO_SLURM_FILE_LOG
export COLLECT_THREAD_TIMELINE_DETAIL ?= 0
export PROCESS_CHANNEL_VERSION=-1
export NO_MPI_DIRECT_RUN ?= 0
export TSAN ?= 0
export ASAN ?= 0
export MSAN ?= 0
export UBSAN ?= 0
export VALGRIND_MODE ?= 0
export ENABLE_THREAD_LOGGER ?= 0
export JSON_STATS_DIR ?= runs/temp_latest
export USE_JEMALLOC ?= 0
export USE_ASMLIB ?= 0
export USE_TRACEANALYZER
export SHOW_SKIPPED_INTERVALS ?= 0
# don't include intervals that end after the end-to-end timer starts or start after the end-to-end timer ends
export TRUNCATE_INTERVALS_OUTSIDE_ETE ?= 1
export TIMER_INTERVAL_NUM_GUESS
export PURE_ROOT ?= $(CPL)
export MAP_PROFILE ?= 0
export MPI_HOSTFILE
export PURE_APP_TIMER ?= 0
export OBJ_DIR_DEBUG_VERBOSE ?= 0
export PURE_RANK_0_STATS_ONLY ?= 0
export ENABLE_HYPERTHREADS

# export OMP_NUM_THREADS ?= 1
# export OMP_PROC_BIND ?= close
# export OMP_PLACES ?= cores
# export OMP_DISPLAY_ENV=verbose

export TOTAL_THREADS ?= $(TOTAL_MPI_THREADS)	

# change MPI runtime name to include DMAPP if there
ifeq ($(USE_MPI_DMAPP), 1)
	RUNTIME = MPI_DMAPP
else
	RUNTIME = MPI
endif

# we changed name from USER_CLEAN_COMMANDS to EXTRA_CLEAN_TEST_COMMANDS. This handles old tests written the old way
EXTRA_CLEAN_TEST_COMMANDS ?= $(USER_CLEAN_COMMANDS)

SUPPORT_PATH = $(CPL)/support

include $(SUPPORT_PATH)/Makefile_includes/Makefile.build_setup.mk
include $(SUPPORT_PATH)/Makefile_includes/Makefile.sanitizer_opts.mk

COMMON_C_FLAGS += -Wall
CFLAGS += $(COMMON_C_FLAGS) $(USER_CFLAGS)
CXXFLAGS += $(COMMON_C_FLAGS) $(USER_CXXFLAGS) $(USER_CPPFLAGS)
LIBS += -lpthread 

# all cflags and cxxflags for the program must be defined above this point
include $(SUPPORT_PATH)/Makefile_includes/Makefile.misc.mk
include $(SUPPORT_PATH)/Makefile_includes/Makefile.application.mk

INCLUDES += -I$(PURE_ROOT)/include/ 

ifeq ($(TRACE_MPI_CALLS),1)
	ifeq ($(SPECIFIED_TRACED_SRCS_FOR_TRACING_MODE),)
		# change sources names from .cpp to .traced.cpp
		TRACED_SRCS := $(patsubst %.c, %.traced.c, $(BENCH_SRCS))
		TRACED_SRCS := $(patsubst %.cpp, %.traced.cpp, $(TRACED_SRCS))
	else
		SRCS = $(SPECIFIED_UNTRACED_SRCS_FOR_TRACING_MODE) 
		TRACED_SRCS += $(SPECIFIED_TRACED_SRCS_FOR_TRACING_MODE)
	endif
	SRCS += $(TRACED_SRCS)
else
	SRCS = $(BENCH_SRCS)
endif

SRC_ROOT = $(PURE_ROOT)/src/
SRCS +=	$(SRC_ROOT)/support/stats_generator.cpp $(SRC_ROOT)/support/thread_logger.cpp \
        $(SRC_ROOT)/support/helpers.cpp $(SRC_ROOT)/support/zed_debug.cpp \
        $(SRC_ROOT)/support/benchmark_timer.cpp 
OBJS = $(patsubst %, $(BUILD_TEST_DIR)/%.o, $(SRCS))

#################################################################

.PRECIOUS: %.traced.cpp %.traced.c
.PHONY: clean clean_run clean_test clean_annotator_files app_prebuild run raw-run vars depend all main-prebuild
.DEFAULT: all_default
.DEFAULT_GOAL: all_default

all_default: main-prebuild
	@echo
	@echo -e "$(KLTBLUE)__ Compilation of $(MAIN) complete.$(KRESET)"
	@echo -e "$(KLTCYAN)   [branch: $(shell git rev-parse --abbrev-ref HEAD) $(shell git rev-parse --short HEAD)]        ___________________________________________$(KRESET)"
	@echo

main-prebuild: app_prebuild .verify_obj_dir cat_build_opts
	$(MAKE) $(BIN_NAME)
	$(MAKE) codesign_app

$(BIN_NAME): $(EXTRA_USER_DEPFILES) $(OBJS)
	$(CXX) $(CXXFLAGS) -o $(BIN_NAME) $(OBJS) $(LFLAGS) $(LIBS)

# test PIPESTATUS[0] checks output of running program; N.B. http://stackoverflow.com/questions/1221833/bash-pipe-output-and-capture-exit-status
run: ALL_ANNOTATIONS = logs/__annotator_outfile.csv
run: main-prebuild numactl_status clean_annotator_files
	$(RM_RF) $(JSON_STATS_DIR)/*
	@echo "████████████████████████████████████████████████████████████████████████████████████"
	JSON_STATS_DIR=$(JSON_STATS_DIR) $(RUN_CMD) $(INTERACTIVE_SUFFIX)
	@echo "████ SUCCESS (exit code 0) █████████████████████████████████████████████████████████"
	@JSON_STATS_DIR=$(JSON_STATS_DIR) make app_postsuccess
	$(MAKE) FILE_TO_REMOVE_ANSI_FROM=$(RUN_OUTPUT_FILENAME) remove_ansi_codes
ifneq ($(TRACED_SRCS),)
	$(RM_F) $(ALL_ANNOTATIONS) # clear out old one
	echo "file:line, function, MPI_function, sender_rank, receiver_rank, msg_len, datatype, tag, datatype_bytes" > $(ALL_ANNOTATIONS) 
	cat logs/__annotator_outfile_*.csv >> $(ALL_ANNOTATIONS) 
	@echo "Finished tracing non-pure MPI program. See below to open CSV in excel."
	@echo "excel $(ALL_ANNOTATIONS)"
endif

%.traced.cpp: %.cpp $(EXTRA_USER_DEPFILES)
	@echo "creating traced cpp source $@"
	$(SUPPORT_PATH)/runtime/mpi_runtime_tracer_annotate.rb -i $<

%.traced.c: %.c $(EXTRA_USER_DEPFILES)
	@echo "creating traced c source $@"
	$(SUPPORT_PATH)/runtime/mpi_runtime_tracer_annotate.rb -i $<

$(BUILD_TEST_DIR)/%.cpp.o: %.cpp $(EXTRA_USER_DEPFILES)
	$(shell [ -d $(shell dirname $@) ] || mkdir -p $(shell dirname $@) )
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(USER_LIB) -c $<  -o $@

$(BUILD_TEST_DIR)/%.c.o: %.c $(EXTRA_USER_DEPFILES)
	$(shell [ -d $(shell dirname $@) ] || mkdir -p $(shell dirname $@) )
	$(CC) $(CFLAGS) $(INCLUDES) $(USER_LIB) -c $<  -o $@

# assembly files only
%.cpp.s: %.cpp $(EXTRA_USER_DEPFILES)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(USER_LIB) -S $<

%.c.s: %.c $(EXTRA_USER_DEPFILES)
	$(CC) $(CFLAGS) $(INCLUDES) $(USER_LIB) -S $<

clean: clean_annotator_files clean_traced_sources clean_timer_intervals
	$(RM_F) $(BIN_NAME) $(OBJS) $(BIN_NAME).log
ifneq ($(strip $(EXTRA_CLEAN_TEST_COMMANDS)),"")
	$(EXTRA_CLEAN_TEST_COMMANDS)
endif

# nonpure applications don't have a lib directory so defining this one separately from the Pure one
clean_build_prefix: clean 
ifndef BUILD_EXTRA_PREFIX
	$(error BUILD_EXTRA_PREFIX must be defined to call $@)
endif
	$(RM_RF) $(BUILD_DIR)/$(BUILD_EXTRA_PREFIX)/*

clean_all: clean
	$(RM_F) $(BIN_PREFIX)@*

clean_run:
	$(MAKE) clean
	$(MAKE) run

clean_annotator_files:
	$(RM_F) logs/__annotator_outfile*.csv

clean_traced_sources:
ifneq ($(TRACED_SRCS),)
	$(RM_F) $(TRACED_SRCS)
endif

clean_timer_intervals:
	$(RM_F) timer_interval/*

analyze: CXXFLAGS += $(CXXFLAGS_FOR_ANALYZE)
analyze:
	@echo "Statically analyzing code with some checking options. See all warning options at https://gcc.gnu.org/onlinedocs/gcc/Warning-Options.html"
	$(MAKE) clean
	$(MAKE)

tidy-base:
	clang-tidy -checks=* $(TIDY_SRCS) -- $(CXXFLAGS) $(INCLUDES) > $(TIDY_LOG)
	@echo "See clang-tidy output on $(TIDY_SRCS) in $(TIDY_LOG) in $(TEST_DIR)"

tidy-test: TIDY_SRCS = $(BENCH_SRCS)
tidy-test: tidy-base

tidy: TIDY_SRCS = $(SRCS)
tidy: tidy-base

vars:
	@echo "Makefile Variables for nonpure applications"
	@echo "SRCS: $(SRCS)"
	@echo "BENCH_SRCS: $(BENCH_SRCS)"
	@echo "TRACED_SRCS: $(TRACED_SRCS)"
	@echo "OBJS: $(OBJS)"
	@echo "BIN_NAME: $(BIN_NAME)"
	@echo "DEBUG: $(DEBUG)"
	@echo "CC: $(CC)"
	@echo "CXX: $(CXX)"
	@echo "LFLAGS: $(LFLAGS)"
	@echo "LIBS: $(LIBS)"
	@echo "INCLUDES: $(INCLUDES)"
	@echo "CFLAGS: $(CFLAGS)"
	@echo "CXXFLAGS: $(CXXFLAGS)"
	@echo "RUN_ARGS: $(RUN_ARGS)"
	@echo "ENABLE_THREAD_LOGGER: $(ENABLE_THREAD_LOGGER)"
	@echo "USER_GDB_COMMANDS: $(USER_GDB_COMMANDS)"
	@echo "NPROCS: $(NPROCS)"
	@echo "BUILD_PREFIX: $(BUILD_PREFIX)"
	@echo "PURE_ROOT: $(PURE_ROOT)"

	@echo "SLURM_CMD: $(SLURM_CMD)"
	@echo "NUMACTL_CPUS: $(NUMACTL_CPUS)"
	@echo "NUMACTL_DESC: $(NUMACTL_DESC)"
	@echo "MPIRUN_OPTS: $(MPIRUN_OPTS)"
	@echo "TRACE_MPI_CALLS: $(TRACE_MPI_CALLS)"
	@echo "SPECIFIED_SRCS_FOR_TRACING_MODE: $(SPECIFIED_SRCS_FOR_TRACING_MODE)"
	@echo "TRACED_SRCS: $(TRACED_SRCS)"
	@echo "TEST_DIR: $(TEST_DIR)"
	@echo "BUILD_TEST_DIR: $(BUILD_TEST_DIR)"
	@echo
	@echo "RUN_CMD: $(RUN_CMD)"
	@echo


.PHONY: list-targets
list-targets:
	@$(MAKE) -pRrq -f /Users/jim/Documents/Research/projects/pure_trees/rt_buffer/hybrid_programming/pure/support/Makefile_includes/Makefile.nonpure.mk : 2>/dev/null | awk -v RS= -F: '/^# File/,/^# Finished Make data base/ {if ($$1 !~ "^[#.]") {print $$1}}' | sort | egrep -v -e '^[^[:alnum:]]' -e '^$@$$' | xargs

depend:
	@echo "WARNING: Calling depend target for nonpure, which is not configured yet."

# get extra targets related to valgrind
EXTRA_DEBUGINFO_PATH ?= 
VALGRIND_LOG_DIR ?= "valgrind"

include $(SUPPORT_PATH)/Makefile_includes/Makefile.valgrind_targets.mk
include $(SUPPORT_PATH)/Makefile_includes/Makefile.profiling.mk
include $(SUPPORT_PATH)/Makefile_includes/Makefile.extra_targets.mk
