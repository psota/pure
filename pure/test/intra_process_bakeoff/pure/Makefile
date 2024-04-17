TOTAL_THREADS ?=	2
export PURE_NUM_PROCS      ?= AUTO
export PURE_RT_NUM_THREADS ?= AUTO

NUM_ITERATIONS ?=  10000
PAYLOAD_COUNT ?= 2000000
RUN_ARGS ?= $(NUM_ITERATIONS) $(PAYLOAD_COUNT)

#LINK_TYPE ?= static

# must make clean (or make clean_run) is you change process channel configuration:
PROCESS_CHANNEL_VERSION ?= 40
PCV_4_NUM_CONT_CHUNKS ?= 1
#PROCESS_CHANNEL_BUFFERED_MSG_SIZE ?= 1048576
#PROCESS_CHANNEL_BUFFERED_MSG_SIZE ?= 4096
PROCESS_CHANNEL_BUFFERED_MSG_SIZE ?= 512

BUFFERED_CHAN_MAX_PAYLOAD_BYTES ?= 4096

PRINT_PROCESS_CHANNEL_STATS ?= 0
COLLECT_THREAD_TIMELINE_DETAIL ?= 0

USE_RDMA_MPI_CHANNEL ?= 0
PURE_RDMA_MODE ?= 1

ENABLE_HYPERTHREADS ?= 1

#NUMA_SCHEME ?= bind_sequence
#NUMA_SCHEME ?= hyperthread_siblings
NUMA_SCHEME ?= different_numa
#NUMA_SCHEME ?= none
#NUMA_SCHEME ?= shared_l3

#OVERRIDE_RUNTIME ?= PureProcPerNUMAMilan

MAX_BYTES_FOR_INTRANODE_QUEUE_STYLE ?= 8192
LINK_TYPE ?= static

######################################################

DEBUG   ?= 0
PROFILE ?= 1
PAUSE_FOR_DEBUGGER_ATTACH ?= 1

TSAN ?= 0
ASAN ?= 0
VALGRIND ?= 0

######################################################

USER_LFLAGS =
USER_LIBS =
USER_CFLAGS = 
USER_CXXFLAGS = -DBUFFERED_CHAN_MAX_PAYLOAD_BYTES=$(BUFFERED_CHAN_MAX_PAYLOAD_BYTES)
USER_INCLUDES =

define USER_GDB_COMMANDS
#break intra_process_bakeoff_pure.not-logged.purified.cpp:32
#break bundle_channel_endpoint.h:162
#break bundle_channel_endpoint.h:166
endef


PURE_USER_CODE_SRCS = intra_process_bakeoff_pure.cpp
NON_PURE_SRCS =
BIN_NAME = intra_process_bakeoff_pure


include ../../Makefile.include.mk
