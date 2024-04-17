export NPROCS ?= 2
export PURE_NUM_PROCS ?= $(NPROCS)
export PURE_RT_NUM_THREADS ?= 1

NUM_ITERATIONS ?=  10000
PAYLOAD_COUNT ?= 2
RUN_ARGS ?= $(NUM_ITERATIONS) $(PAYLOAD_COUNT)

NUMA_SCHEME ?= shared_l3
#NUMA_SCHEME ?= hyperthread_siblings
#NUMA_SCHEME ?= different_numa
#NUMA_SCHEME ?= none

ENABLE_HYPERTHREADS ?= 0

export DEBUG ?= 1
export PROFILE ?= 0
export VALGRIND_MODE ?= 0
export USER_LFLAGS ?=
export ENABLE_THREAD_LOGGER ?= 0

export TSAN ?= 0
export ASAN ?= 0
export MSAN ?= 0
export MPIP ?= 0
export TRACE_COMM ?= 0
export SANITIZER_LOG_DIR ?= sanitizer

TEST_DIR := $(shell pwd)
BIN_NAME = intra_process_bakeoff
BENCH_SRCS = intra_process_bakeoff.cpp
USER_LIB = 
SHELL = /bin/bash
DIRS = 


# configure ability to use Pure resources, and define some basic rules (clean, run, $(BIN_NAME), etc.)
VALGRIND_LOG_DIR ?= valgrind
include $(CPL)/support/Makefile_includes/Makefile.nonpure.mk
