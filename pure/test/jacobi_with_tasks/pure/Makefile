TOTAL_THREADS ?=	32
export PURE_NUM_PROCS      ?= AUTO
export PURE_RT_NUM_THREADS ?= AUTO

export PRINT_NODE_CONFIG ?= 0
DO_SENDRECV   ?= 1
DO_COMPUTATION ?= 1

#OVERRIDE_RUNTIME ?= PureProcPerNUMA

ITER ?= 200000000
CLS_PER_RANK ?= 64
RUN_ARGS ?= $(ITER) $(CLS_PER_RANK)
# ITER_WEIGHT ?= 10000000
ITER_WEIGHT ?= 100


# only relevant on cagnodes
#export MPI_HOSTFILE += hostfile

#NUMA_SCHEME ?= hyperthread_siblings
NUMA_SCHEME ?= bind_sequence


DEBUG ?= 0
PAUSE_FOR_DEBUGGER_ATTACH ?= 0
PROFILE ?= 1
ENABLE_THREAD_LOGGER ?= 0
VALGRIND_MODE ?= 0
USER_LFLAGS ?=
ENABLE_THREAD_LOGGER ?= 0
DISABLE_PURIFICATION ?= 1
ENABLE_HYPERTHREADS  ?= 1

PROCESS_CHANNEL_VERSION  ?= 40
PCV_4_NUM_CONT_CHUNKS    ?= $(CLS_PER_RANK)

TSAN ?= 0
ASAN ?= 0
MSAN ?= 0
MPIP ?= 0
TRACE_COMM ?= 0
TRACE_MPI_CALLS ?= 0
SANITIZER_LOG_DIR ?= sanitizer
USE_ASMLIB ?= 0

# fixme
USE_JEMALLOC ?= 1
MAX_DIRECT_CHAN_BATCHER_MESSAGE_PAYLOAD_BYTES = 0
LINK_TYPE ?= static
PROCESS_CHANNEL_BUFFERED_MSG_SIZE ?= 8
PRINT_PROCESS_CHANNEL_STATS ?= 0
COLLECT_THREAD_TIMELINE_DETAIL ?= 0

# vet
MAX_BYTES_FOR_INTRANODE_QUEUE_STYLE ?= 8192
BUFFERED_CHAN_MAX_PAYLOAD_BYTES ?= 8192

# PROFILE is for both mpiP and perf_events profiling. The make target determines which.
APP_DEBUG ?= 0
APP_DEBUG_VERBOSE ?= 0
ALLOW_INVALID_RESULT ?= 0

__FLAGS = -DITER_WEIGHT=$(ITER_WEIGHT) -DDO_COMPUTATION=$(DO_COMPUTATION)
USER_CFLAGS   = $(__FLAGS)
USER_CXXFLAGS = $(__FLAGS)


TEST_DIR := $(shell pwd)
BIN_NAME = rand_jacobi_pure.$(ITER_WEIGHT)
SHELL = /bin/bash

PURIFIED_SOURCES =

ifeq ($(DO_SENDRECV),1)
	NON_PURIFIED_SOURCES = rand_jacobi_pure.cpp
else
	NON_PURIFIED_SOURCES = rand_jacobi_pure_no_send_recv.cpp
# 	NON_PURIFIED_SOURCES = hack.cpp
endif

PURE_USER_CODE_SRCS = $(PURIFIED_SOURCES) $(NON_PURIFIED_SOURCES)

include ../../Makefile.include.mk
