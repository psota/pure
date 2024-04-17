DO_OPENMP ?= 0

ifeq ($(DO_OPENMP),1)
export NPROCS ?= 1
OMP_NUM_THREADS ?= 32
NUMA_SCHEME = none
export OMP_PLACES ?= {0:16},{32:16},{16:16},{48:16}
else
export NPROCS ?= 256
#export NUMA_SCHEME ?= bind_sequence
USE_MPI_DMAPP ?= 0
endif


# ampi setup
#NERSC_COMPILER_OVERRIDE ?= ampi
#AMPI_PHYS_PROCS ?= 8
#AMPI_OPTS       ?= +vp$(NPROCS)


ITER ?= 10000
RUN_ARGS ?= $(ITER)

#NUMA_SCHEME ?= hyperthread_siblings
#NUMA_SCHEME ?= none
# NUMA_SCHEME ?= bind_sequence

export DEBUG   ?= 1
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

USER_CXXFLAGS ?= -DDO_OPENMP=$(DO_OPENMP) -DPAYLOAD_COUNT=$(PAYLOAD_COUNT)
ifeq ($(DO_OPENMP),1)
USER_CXXFLAGS += -fopenmp 
endif


TEST_DIR := $(shell pwd)
BIN_NAME = barrier_test-$(DO_OPENMP)
BENCH_SRCS = barrier_test.cpp
USER_LIB = 
SHELL = /bin/bash
DIRS = 

# configure ability to use Pure resources, and define some basic rules (clean, run, $(BIN_NAME), etc.)
VALGRIND_LOG_DIR ?= valgrind

include $(CPL)/support/Makefile_includes/Makefile.nonpure.mk
