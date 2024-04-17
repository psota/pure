NPROCS ?= 4
PURE_NUM_PROCS ?= $(NPROCS)
PURE_RT_NUM_THREADS ?= 1

ARRAY_ELTS ?= 60
ITER ?= 1
RUN_ARGS ?= $(ITER) $(ARRAY_ELTS)
ITER_WEIGHT ?= 10000000

# only relevant on cagnodes
#export MPI_HOSTFILE += hostfile

#NUMA_SCHEME ?= hyperthread_siblings
#NUMA_SCHEME ?= bind_alternating
#NUMA_SCHEME ?= none

DEBUG ?= 1
PROFILE ?= 0
ENABLE_THREAD_LOGGER ?= 0
VALGRIND_MODE ?= 0
USER_LFLAGS ?=
ENABLE_THREAD_LOGGER ?= 0

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
ALLOW_INVALID_RESULT ?= 0

__FLAGS = -DITER_WEIGHT=$(ITER_WEIGHT)
USER_CFLAGS   = $(__FLAGS)
USER_CXXFLAGS = $(__FLAGS)


TEST_DIR := $(shell pwd)
BENCH_SRCS = rand_jacobi.cpp
BIN_NAME = rand_jacobi.$(ITER_WEIGHT)
SHELL = /bin/bash

include $(CPL)/support/Makefile_includes/Makefile.nonpure.mk
