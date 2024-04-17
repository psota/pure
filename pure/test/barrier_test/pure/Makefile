TOTAL_THREADS ?= 256
PURE_NUM_PROCS ?= AUTO
PURE_RT_NUM_THREADS ?= AUTO
#MPI_PROCS_PER_NODE_LIMIT ?= 1
#NO_SLURM_FILE_LOG ?= 0

# if ENABLE_THREAD_AFFINITY_AUTOMAPPER is set to 1, the build system will automatically look for a file named
# THREAD_AFFINITY_AUTOMAPPER_FILENAME. If it's found, it uses it. If it's not found, it throws an error.
# This allows other scripts to just set ENABLE_THREAD_AFFINITY_AUTOMAPPER and have the filename be automatically
# determined by the following defintion.
# ENABLE_THREAD_AFFINITY_AUTOMAPPER ?= 0
# THREAD_AFFINITY_AUTOMAPPER_FILENAME := threadmaps/$(CLASS)_$(TOPO)_p$(PURE_NUM_PROCS)_t$(PURE_RT_NUM_THREADS).threadmap
#THREAD_MAP_FILENAME ?= A_BH_p3_t7.threadmap

# Some CoMD-Makefile-specific configurations
### CoMD can be built in either double or single precision and with or
### without MPI.  Select desired precision and MPI here.
#export MPI_HOSTFILE = hostfile.lanka.auto

# APP_PREBUILD_MAKEFILE = 

######################################################

ITER ?= 100
RUN_ARGS ?= $(ITER)

DO_V1_BARRIER ?= 0
DO_V2_BARRIER ?= 1


ENABLE_HYPERTHREADS ?= 0


#OVERRIDE_RUNTIME ?= PureProcPerNUMA

# only relevant on cagnodes
#export MPI_HOSTFILE += hostfile

#NUMA_SCHEME ?= hyperthread_siblings
# is this relevant in this benchmark?
NUMA_SCHEME ?= bind_sequence
#NUMA_SCHEME ?= none

DEBUG ?= 1
PAUSE_FOR_DEBUGGER_ATTACH ?= 0
PROFILE ?= 0
ENABLE_THREAD_LOGGER ?= 0
VALGRIND_MODE ?= 0
USER_LFLAGS ?=
ENABLE_THREAD_LOGGER ?= 0
PCV_4_NUM_CONT_CHUNKS ?= 1
MAX_BYTES_FOR_INTRANODE_QUEUE_STYLE ?= 1024 

TSAN ?= 0
ASAN ?= 0
MSAN ?= 0
MPIP ?= 0
TRACE_COMM ?= 0
TRACE_MPI_CALLS ?= 0
SANITIZER_LOG_DIR ?= sanitizer

# PROFILE is for both mpiP and perf_events profiling. The make target determines which.
APP_DEBUG ?= 0
APP_DEBUG_VERBOSE ?= 0
ALLOW_INVALID_RESULT ?= 1
LINK_TYPE ?= static

PROCESS_CHANNEL_VERSION ?= 40
PROCESS_CHANNEL_BUFFERED_MSG_SIZE ?= 8
PRINT_PROCESS_CHANNEL_STATS ?= 0

TEST_DIR := $(shell pwd)
PURE_USER_CODE_SRCS = barrier_test.cpp
BIN_NAME = barrier_test

SHELL = /bin/bash

USER_CXXFLAGS = -DDO_V1_BARRIER=$(DO_V1_BARRIER) -DDO_V2_BARRIER=$(DO_V2_BARRIER)

# EXTRA_CLEAN_TEST_COMMANDS =

include ../../Makefile.include.mk
