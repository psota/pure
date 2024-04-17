# Author: James Psota
# File:   Makefile.misc.mk 

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

RM = rm
RM_F = rm -f
RM_RF = rm -rf

# override relative paths from old baseline makefiles
PURE_ROOT := $(CPL)
export USE_FAST_MATH ?= 0

MAKEFILE_PATH := $(abspath $(lastword $(MAKEFILE_LIST)))
# MAKEFILE_DIR is a misnomer -- its the PURE makefile dir for Makefile.include.mk
export MAKEFILE_DIR := $(CPL)/test/
export CURRENT_DIR := $(notdir $(patsubst %/,%,$(dir $(MAKEFILE_PATH))))

# must be included in this order
export MPIP = -f $(MPIP_LOG_DIR) -p -k5

export TEST_DIR := $(shell pwd)
GDB_SCRIPT ?= commands.gdb
export VALGRIND_LOG_DIR ?= valgrind
export QUICK_DEBUG_JOB
DEPFILE	= Makefile.depend
export TIMESTAMP = $(shell date +%Y%m%d%H%M%S)

export USER_CXXFLAGS
export USER_CFLAGS

# default
USE_MPI_DMAPP ?= 0

ifneq ($(shell $(CC) --version | grep clang),)
	IS_CLANG := 1
else
	IS_CLANG := 0
endif

# host configuration
ifeq ($(OS),osx)
	# OSX-specific items
	#export MPICH_UTIL_DIR=/Users/jim/local/src/mpich-3.1.4/src/util

	# current version of clang doesn't support restrict keyword. eliding it for osx for now. 
	# TODO: update clang on laptops
	#CXXFLAGS += -Drestrict= 

	# LOCAL_INCLUDE = /Users/jim/local/include/
	# MAKEDEPEND_INCLUDES += -I/usr/include/c++/4.2.1

	#LOCAL_INCLUDE = $(TOOLCHAIN_PREFIX)/
	BOOST_INCLUDE = /Users/jim/local/src/boost_1_63_0
	BOOST_LIB_DIR = /Users/jim/local/src/boost_1_63_0/stage/lib

	# https://gcc.gnu.org/onlinedocs/gcc-4.9.2/gcc/Language-Independent-Options.html
	CXXFLAGS += -fdiagnostics-color=always
	CXXFLAGS += -DUSING_CRAYMPICH_LARGE_TAG=0
	
	# https://stackoverflow.com/questions/77133361/no-template-named-unary-function-in-namespace-std-did-you-mean-unary-fun
	CXXFLAGS += -D_LIBCPP_ENABLE_CXX17_REMOVED_UNARY_BINARY_FUNCTION 

	# via folly/detail/CacheLocality.h
	# /// Memory locations on the same cache line are subject to false
	# /// sharing, which is very bad for performance.  Microbenchmarks
	# /// indicate that pairs of cache lines also see interference under
	# /// heavy use of atomic operations (observed for atomic increment on
	# /// Sandy Bridge).  See FOLLY_ALIGN_TO_AVOID_FALSE_SHARING
	CXXFLAGS += -DCACHE_LINE_BYTES=64
	CXXFLAGS += -DFALSE_SHARING_RANGE=128
	CXXFLAGS += -DENABLE_HYPERTHREADS=$(ENABLE_HYPERTHREADS)

	NPROC ?= $(shell sysctl -n hw.ncpu)

	MPICH_PATH = /opt/homebrew
	CC = $(MPICH_PATH)/bin/mpicc
	CXX = $(MPICH_PATH)/bin/mpic++
	MPIRUN = $(MPICH_PATH)/bin/mpirun
	LFLAGS += -L$(MPICH_PATH)/lib

	# hacking in this nullabilty fix for now 

	# I don't know why I need to explicitely include the C++ headers
	__common_flags = -DOSX -I/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include -Wno-nullability-completeness -I/opt/homebrew/Cellar/jsoncpp/1.9.5/include
	CFLAGS   += $(__common_flags) 
	CXXFLAGS += -I/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/c++/v1 $(__common_flags) 
	LFLAGS += -L/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/lib 

	# special directory for brew-installed libjsoncpp - make more robust.


	LFLAGS += -L/opt/homebrew/Cellar/jsoncpp/1.9.5/lib

endif

ifeq ($(OS), $(filter $(OS), cag nersc))
	# Linux stuff
	CXXFLAGS += -DLINUX
	LIBS += -lnuma

	ifeq ($(OS),nersc)

		# defaults
		CC=cc
		CXX=CC

# delete on july 8
# 		ifeq ($(NERSC_COMPILER_OVERRIDE),intel)
# # do these not work on pmut?
# # 			CC=mpiicc
# # 			CXX=mpiicpc
# 		endif


		# super brittle (will break when compiler ceases to be gcc 8.2) -- update -- using this path should be better
		#/global/common/sw/cray/cnl7/haswell/numactl/2.0.12/gcc/8.2.0/lv3fmx3/lib 
		LFLAGS += -L$(LIBNUMA_LIB_PATH)

		# testing, this may help fix but with Intel tools
		LIBS += -latomic

		# special Cray optimizations -- only supported for Haswell for now
# 		ifeq ($(CORI_ARCH), craype-haswell)	
# 			ifeq ($(USE_MPI_DMAPP), 1)
# 				ifeq ($(LINK_TYPE), static)
# 					LIBS += -Wl,--whole-archive,-ldmapp,--no-whole-archive
# 				else 
# 					LIBS += -ldmapp
# 				endif
# 				export MPICH_USE_DMAPP_COLL=1
# 				export MPICH_RMA_OVER_DMAPP=1
# 			else
# 				export MPICH_USE_DMAPP_COLL=0
# 				export MPICH_RMA_OVER_DMAPP=0
# $(warning MPICH DMAPP disabled with USE_MPI_DMAPP flag)		
# 			endif
# 		else
# #$(warning Not loading DMAPP for KNL architecture. Only 123 "FMAs" are supported but we like to use more cores.)
# 		endif

		CXXFLAGS += -DCACHE_LINE_BYTES=64
		CXXFLAGS += -DFALSE_SHARING_RANGE=128

		ifeq ($(LARGE_MPI_TAG),1)
			# special large tag size
			#   If the code uses static linking:
			#                  cc -craympich-dpm -o mpi_test.x mpi_test.c
			#                  If the code uses dynamic linking:
			#                  cc -dynamic -craympich-dpm -o mpi_test.x mpi_test.c
			CXXFLAGS += -dynamic -craympich-dpm -DUSING_CRAYMPICH_LARGE_TAG=1
		else
			CXXFLAGS += -DUSING_CRAYMPICH_LARGE_TAG=0
		endif


# removing for
# 		ifeq ($(PROFILE), 1)
# 			# special options for VTune -- see https://software.intel.com/en-us/vtune-help-analyzing-statically-linked-binaries-on-linux-targets
# 			# see https://github.com/Intel-Media-SDK/MediaSDK/issues/34 for no-as-needed flag
# 			LFLAGS += -Wl,--no-as-needed -ldl -Wl,-upthread_getattr_np -Wl,-upthread_attr_getstack -Wl,-upthread_attr_getstacksize -Wl,-upthread_attr_setstack, -Wl,-upthread_attr_setstacksize -Wl,-udlopen -Wl,-udlclose -Wl,-udlsym
# 		endif

		# On sept. 19, this was needed to get runs on NERSC to work. Removing as it may have been transient. See NERSC incident INC0127179 if this arises again.
		# the issue is that -dynamic is needed on the compute nodes (but not login nodes)
		# CFLAGS += -dynamic
		# CXXFLAGS += -dynamic

		BOOST_INCLUDE = $(BOOST_DIR)/include

		# make this overrideable because nproc on the nersc machines doesn't return the right value
		# note: this is assuming we are reserving the entire node, which we almost always do.
		ifdef SLURM_CPUS_ON_NODE
			# SLURM job (interactive or batch)
			NPROC = $(SLURM_CPUS_ON_NODE)
		else
			# login node
			# TODO: this could be a bug with parallel make on login nodes. Consider changing.
			NPROC = 16
		endif

		ifeq ($(MAP_PROFILE), 1)
$(warning Compiling with Alinea MAP profiling library -- NERSC profile mode.)
			 LIBS += -Wl,@$(TEST_DIR)/allinea-profiler.ld
		endif

# 		ifneq ($(USE_TRACEANALYZER),)
# 			# intel trace analyzer
# # 			#LFLAGS += -L$(VT_LIB_DIR) -shared  
# # 			#LFLAGS += -L$(VT_LIB_DIR)
# # 			ifeq ($(VT_LIB_DIR),)
# # $(error "To run TRACE_ANALYZER, VT_LIB_DIR must be defined")
# # 			endif
# # $(warning Using Intel Trace Analyzer Library $(USE_TRACEANALYZER))
# # 			LIBS += $(VT_ADD_LIBS) -l$(USE_TRACEANALYZER)

# 			# CXXFLAGS += -dynamic -fPIC
# 			# CFLAGS += -dynamic -fPIC

# 		endif

		# TODO: incorporate feedback from: http://www.nersc.gov/users/computational-systems/cori/programming/compiling-codes-on-cori/
	endif

	ifeq ($(OS),cag)
		# CAG-specific items
		# atomic possibly necessary for atomic<PureContRetT> - https://gcc.gnu.org/bugzilla/show_bug.cgi?id=65756
		LIBS += -lbsd -latomic

		export MPICH_UTIL_DIR=/afs/csail.mit.edu/u/j/jim/local/src/mpich-3.2/src/util

 		LOCAL_INCLUDE = /afs/csail.mit.edu/u/j/jim/local/include
 		LOCAL_LIB = /afs/csail.mit.edu/u/j/jim/local/lib
		TOOLCHAIN_DIR = /data/scratch/jim/toolchains

		NPROC ?= $(shell nproc)

		ifeq ($(MPI_ENABLED), 1)
			# automatically add this in
			MPI_HOSTFILE ?= mpi_hostfile
		endif

		ifneq ($(MPI_HOSTFILE),)
			MPIRUN_OPTS += -f $(MPI_HOSTFILE)
		endif

		# fixme -- these includes are way out of date. 

		MAKEDEPEND_INCLUDES += -I$(TOOLCHAIN_DIR)/gcc_8.3.0/include/c++/8.3.0
		#MAKEDEPEND_INCLUDES += -I$(LOCAL_INCLUDE)

		BOOST_INCLUDE = /afs/csail.mit.edu/u/j/jim/local/src/boost_1_63_0
		BOOST_LIB_DIR = /afs/csail.mit.edu/u/j/jim/local/src/boost_1_63_0/libs
		CXXFLAGS += -DCACHE_LINE_BYTES=64
		CXXFLAGS += -DFALSE_SHARING_RANGE=128
		CXXFLAGS += -DUSING_CRAYMPICH_LARGE_TAG=0

		#CXXFLAGS += --gcc-toolchain=$(TOOLCHAIN_DIR)/gcc-8.2.0

		# add BSD path (not built in by default on lanka)
		LFLAGS += -L$(LOCAL_LIB)

		# configure paths and compiler
		ifeq ($(DEBUG),1)

			MPICH_PATH=$(TOOLCHAIN_DIR)/mpich/debug
			CC=$(MPICH_PATH)/bin/mpicc-debug
			CXX=$(MPICH_PATH)/bin/mpic++-debug
			MPIRUN=$(MPICH_PATH)/bin/mpirun-debug
			LFLAGS += -L$(MPICH_PATH)/lib
			CFLAGS += -rdynamic

		else ifeq ($(PROFILE),1)
			# keep in sync with debug above (consider using Makefile filter function)
			# consider building a special version which has -O3 and symbols
			MPICH_PATH=$(TOOLCHAIN_DIR)/mpich/debug
			CC=$(MPICH_PATH)/bin/mpicc-debug
			CXX=$(MPICH_PATH)/bin/mpic++-debug
			MPIRUN=$(MPICH_PATH)/bin/mpirun-debug
			LFLAGS += -L$(MPICH_PATH)/lib
		else
			# configuring for release mode
			MPICH_PATH=$(TOOLCHAIN_DIR)/mpich/release
			CC=$(MPICH_PATH)/bin/mpicc-release	
			CXX=$(MPICH_PATH)/bin/mpic++-release
			MPIRUN=$(MPICH_PATH)/bin/mpirun-release
			LFLAGS += -L$(MPICH_PATH)/lib
		endif

		TEMP_WEB_DIR = /afs/csail.mit.edu/u/j/jim/public_html/pure/flamegraphs

	endif

	ifeq ($(NERSC_COMPILER_OVERRIDE),AMPI_base)

		ifeq ($(DEBUG), 1)
# 			CHARM_PATH = $(LOCAL_PREFIX)/src/charm-v7.0.0_debug_no_smp
			# trying different version
			CHARM_PATH = /global/cscratch1/sd/jpsota/scratch_local/src/charm-latest_debug_no_smp
		else
# 			CHARM_PATH = $(LOCAL_PREFIX)/src/charm-v7.0.0_no_smp
			CHARM_PATH = /global/cscratch1/sd/jpsota/scratch_local/src/charm-latest_no_smp
		endif

  	    export AMPI_VERSION=-base
		export RUNTIME_IS_AMPI=1
$(warning ◆◆◆◆◆   Using AMPI non-SMP: CHARM_PATH=$(CHARM_PATH) )
	endif

	ifeq ($(NERSC_COMPILER_OVERRIDE),AMPI_smp)
		ifeq ($(DEBUG), 1)
			CHARM_PATH = /global/cscratch1/sd/jpsota/scratch_local/src/charm-v7.0.0_debug_smp
		else
			CHARM_PATH = /global/cscratch1/sd/jpsota/scratch_local/src/charm-v7.0.0_smp
		endif

  	    export AMPI_VERSION=-smp
		export RUNTIME_IS_AMPI=1
$(warning ◆◆◆◆◆   Using AMPI SMP: CHARM_PATH=$(CHARM_PATH) )
	endif

	ifeq ($(RUNTIME_IS_AMPI), 1)
		CC  = $(CHARM_PATH)/bin/ampicc -use-new-std
		CXX = $(CHARM_PATH)/bin/ampiCC -use-new-std
		CFLAGS   += -DAMPI_MODE_ENABLED=$(AMPI_VERSION)
		CXXFLAGS += -DAMPI_MODE_ENABLED=$(AMPI_VERSION)
		ifneq ($(DEBUG), 1)
			CFLAGS   += -optimize
			CXXFLAGS += -optimize
		endif
	endif
	
endif

ifneq ($(LOCAL_INCLUDE),)
	INCLUDES += -I$(LOCAL_INCLUDE)
endif

ifneq ($(BOOST_INCLUDE),)
	INCLUDES += -I$(BOOST_INCLUDE)
endif

# Folly include -- in the source directory
INCLUDES += -I$(CPL)/include/pure/3rd_party/folly
CFLAGS   += -DFOLLY_NO_CONFIG
CXXFLAGS += -DFOLLY_NO_CONFIG

# pure header path
INCLUDES += -I$(SUPPORT_PATH)/../include/pure/common

ifeq ($(DEBUG),1)
	__OPTS = -g -ggdb3 -fno-inline -O0 -fno-omit-frame-pointer

	ifeq ($(CHECK_PADDING),1)
		CXXFLAGS += -Wpadded
	endif

	CFLAGS += $(__OPTS)
	CXXFLAGS += $(__OPTS)
	export MALLOC_CHECK_=2

	CFLAGS += -DDEBUG_CHECK=1
	CXXFLAGS += -DDEBUG_CHECK=1

	# https://stackoverflow.com/questions/5945775/how-to-get-more-detailed-backtrace
	LFLAGS += -rdynamic
else ifeq ($(PROFILE),1)
	__OPTS = -g -ggdb3 -fno-omit-frame-pointer
	CFLAGS += $(__OPTS)
	CXXFLAGS += $(__OPTS)

	CFLAGS   += -O3
	CXXFLAGS += -O3

	ifeq ($(OS), nersc)
		CFLAGS   += -march=native
		CXXFLAGS += -march=native
	endif

	CFLAGS   += -DNDEBUG
	CXXFLAGS += -DNDEBUG

	CFLAGS   += -DDEBUG_CHECK=0
	CXXFLAGS += -DDEBUG_CHECK=0

	CFLAGS += -DPROFILE_MODE=1
	CXXFLAGS += -DPROFILE_MODE=1

	# Add special clang optimizations when using clang
	ifeq ($(IS_CLANG), 1)
		ifeq ($(OS), cag)
			# TODO: may have to specially enable this on Cori

			# running clang -- enable advanced features
# 			CFLAGS   += -mllvm -polly -mllvm -polly-vectorizer=stripmine
# 			CXXFLAGS += -mllvm -polly -mllvm -polly-vectorizer=stripmine
		endif
	endif

	# https://stackoverflow.com/questions/5945775/how-to-get-more-detailed-backtrace
	LFLAGS += -rdynamic
else
	# release
	CFLAGS   += -O3 -flto
	CXXFLAGS += -O3 -flto

	ifeq ($(OS), nersc)
		CFLAGS   += -march=native
		CXXFLAGS += -march=native
	endif

	LFLAGS += -flto

	ifeq ($(OS),nersc)
		ifeq ($(NERSC_COMPILER_OVERRIDE),intel)
		# special IPO for optimization of work stealing functions
		# https://software.intel.com/content/www/us/en/develop/documentation/cpp-compiler-developer-guide-and-reference/top/optimization-and-programming-guide/interprocedural-optimization-ipo.html
		CFLAGS += -ipo
		CXXFLAGS += -ipo
		LFLAGS += -ipo
		endif
	endif

	# to turn off assertions in /usr/include/assert.h
	CFLAGS   += -DNDEBUG
	CXXFLAGS += -DNDEBUG

	CFLAGS   += -DDEBUG_CHECK=0
	CXXFLAGS += -DDEBUG_CHECK=0

	# Add special clang optimizations when using clang
	ifeq ($(IS_CLANG), 1)
		# running clang -- enable advanced features
# 		CFLAGS   += -mllvm -polly -mllvm -polly-vectorizer=stripmine
# 		CXXFLAGS += -mllvm -polly -mllvm -polly-vectorizer=stripmine
	endif
endif

ifeq ($(USE_FAST_MATH), 1)
	CFLAGS   += -ffast-math
	CXXFLAGS += -ffast-math
endif

CFLAGS += -std=c11
CXXFLAGS += -std=c++17

ifeq ($(IS_CLANG), 1)
	AR = ar
	LD = ld
else 
	AR = gcc-ar
endif

# use this to run gdb in release mode (-O3) and still get symbols
ifeq ($(FORCE_DEBUGGING_SYMBOLS),1)
	CFLAGS += -g -ggdb3
	CXXFLAGS += -g -ggdb3
endif

ifeq ($(ENABLE_THREAD_LOGGER),1)
	CXXFLAGS += -DENABLE_THREAD_LOGGER=1
endif

ifeq ($(TRACE_COMM),1)
	CXXFLAGS += -DTRACE_COMM=1
endif

# jemalloc config
# https://github.com/jemalloc/jemalloc/wiki/Getting-Started
ifeq ($(USE_JEMALLOC),1)
    
# ifeq ($(DO_JEMALLOC_PROFILE),1)
#     $(error Jim: jemalloc gives an error about an invalid conf string here. FIXME)
#     export MALLOC_CONF="prof:true,prof_accum:false,prof_prefix:jeprof.out"
# endif
    # adding this in to affect the binary name
    CXXFLAGS += -DDO_JEMALLOC_PROFILE=$(DO_JEMALLOC_PROFILE)

    JEMALLOC_FLAGS = -I`jemalloc-config --includedir` -DUSE_JEMALLOC=1
    CFLAGS   += $(JEMALLOC_FLAGS) 
    CXXFLAGS += $(JEMALLOC_FLAGS) 
    LFLAGS += -L`jemalloc-config --libdir` -Wl,-rpath,`jemalloc-config --libdir` -ljemalloc `jemalloc-config --libs`
endif

ifeq ($(USE_ASMLIB),1)
	LFLAGS += -L$(CPL)/include/pure/3rd_party/asmlib
	ifeq ($(OS),osx)
		# use "o" version (at end) to override standard library
		LIBS += -lamac64
	endif
	ifeq ($(OS), nersc)
		# complicated given linking issues with cray. see https://mail.google.com/mail/u/0/#inbox/FMfcgxwGBmpLdPtkTCKRGKJZrqZbPmXv
		LIBS += -laelf64 -Wl,--start-group $(CPL)/include/pure/3rd_party/asmlib/libaelf64.a -Wl,--end-group -zmuldefs
	endif
	ifeq ($(OS), cag)
		LIBS += -laelf64
	endif
	CXXFLAGS += -DUSE_ASMLIB=1
else
	CXXFLAGS += -DUSE_ASMLIB=0
endif

# build libjsoncpp using $(CPL)/support/misc/build_jsoncpp.sh
ifeq ($(OS),osx)
	LIBJSONPATH = /opt/homebrew/Cellar/jsoncpp/1.9.5/lib
else 
	LIBJSONPATH = $(CPL)/src/3rd_party/jsoncpp/build/release/src/lib_json
endif

LFLAGS += -L$(LIBJSONPATH) -Wl,-rpath,$(LIBJSONPATH)
LIBS   += -ljsoncpp

CXXFLAGS += -DPROCESS_CHANNEL_MAJOR_VERSION=$(PROCESS_CHANNEL_MAJOR_VERSION) -DPROCESS_CHANNEL_VERSION=$(PROCESS_CHANNEL_VERSION) 
CXXFLAGS += -DPROCESS_CHANNEL_BUFFERED_MSG_SIZE=$(PROCESS_CHANNEL_BUFFERED_MSG_SIZE)
CXXFLAGS += -DPCV_4_NUM_CONT_CHUNKS=$(PCV_4_NUM_CONT_CHUNKS)
CXXFLAGS += -DBUFFERED_CHAN_MAX_PAYLOAD_BYTES=$(BUFFERED_CHAN_MAX_PAYLOAD_BYTES)
CXXFLAGS += -DPCV4_OPTION_DIRECT_CHANNEL_WORK_STEAL_TRIES=$(PCV4_OPTION_DIRECT_CHANNEL_WORK_STEAL_TRIES)
CXXFLAGS += -DCOLLECT_THREAD_TIMELINE_DETAIL=$(COLLECT_THREAD_TIMELINE_DETAIL)
CXXFLAGS += -DPURE_CORES_PER_NODE=$(shell ruby --disable=gems -e "require ENV['CPL'] + '/support/experiments/benchmarks/machine_helpers.rb' ; puts MachineHelpers.num_cores($(ENABLE_HYPERTHREADS))")
CXXFLAGS += -DDO_PURE_EXHAUST_REMAINING_WORK=$(DO_PURE_EXHAUST_REMAINING_WORK)
CXXFLAGS += -DPURE_PIPUSING_CRAYMPICH_LARGE_TAGUSING_CRAYMPICH_LARGE_TAGELINE_DIRECT_STEAL=$(PURE_PIPELINE_DIRECT_STEAL)
CXXFLAGS += -DUSE_HELPER_THREADS_IF_FREE_CORES=$(USE_HELPER_THREADS_IF_FREE_CORES)
CXXFLAGS += -DPIN_HELPER_THREADS_IF_NUMA_SCHEME=$(PIN_HELPER_THREADS_IF_NUMA_SCHEME)
CXXFLAGS += -DPURE_MAX_HELPER_THREADS=$(PURE_MAX_HELPER_THREADS)
CXXFLAGS += -DPCV4_OPTION_DIRECT_CHANNEL_WORK_STEAL=$(PCV4_OPTION_DIRECT_CHANNEL_WORK_STEAL)
CXXFLAGS += -DPCV4_OPTION_WORK_STEAL_EXHAUSTIVELY=$(PCV4_OPTION_WORK_STEAL_EXHAUSTIVELY)
CXXFLAGS += -DPURE_DMC_TEST_WAIT=$(PURE_DMC_TEST_WAIT)
CXXFLAGS += -DPURE_DMC_TEST_WAIT_DELAY_USECS=$(PURE_DMC_TEST_WAIT_DELAY_USECS)

CXXFLAGS += -DMAX_DIRECT_CHAN_BATCHER_MESSAGE_PAYLOAD_BYTES=$(MAX_DIRECT_CHAN_BATCHER_MESSAGE_PAYLOAD_BYTES)
CXXFLAGS += -DNUM_DIRECT_CHAN_BATCHERS_PER_MATE=$(NUM_DIRECT_CHAN_BATCHERS_PER_MATE)
CXXFLAGS += -DDCB_MAX_MESSAGES_PER_BATCH=$(DCB_MAX_MESSAGES_PER_BATCH)
CXXFLAGS += -DBATCHER_DEBUG_PRINT=$(BATCHER_DEBUG_PRINT)
CXXFLAGS += -DDCB_SEND_BYTES_THRESHOLD=$(DCB_SEND_BYTES_THRESHOLD)
CXXFLAGS += -DDO_LIST_WAITALL=$(DO_LIST_WAITALL)

CXXFLAGS += -DFORCE_MPI_THREAD_SINGLE=$(FORCE_MPI_THREAD_SINGLE)
CXXFLAGS += -DENABLE_MPI_ERROR_RETURN=$(ENABLE_MPI_ERROR_RETURN)
CXXFLAGS += -DUSE_RDMA_MPI_CHANNEL=$(USE_RDMA_MPI_CHANNEL)
CXXFLAGS += -DPURE_RDMA_MODE=$(PURE_RDMA_MODE)
CXXFLAGS += -DPURE_APP_TIMER=$(PURE_APP_TIMER)
CXXFLAGS += -DDO_PRINT_CONT_DEBUG_INFO=$(DO_PRINT_CONT_DEBUG_INFO)
CXXFLAGS += -DPAUSE_FOR_DEBUGGER_ATTACH=$(PAUSE_FOR_DEBUGGER_ATTACH)

# batcher flags
CXXFLAGS += -DENABLE_PURE_FLAT_BATCHER=$(ENABLE_PURE_FLAT_BATCHER) -DFLAT_BATCHER_RECV_VERSION=$(FLAT_BATCHER_RECV_VERSION) \
-DFLAT_BATCHER_SEND_BYTES_THRESHOLD=$(FLAT_BATCHER_SEND_BYTES_THRESHOLD) -DSENDER_BATCHER_DEBUG_PRINT=$(SENDER_BATCHER_DEBUG_PRINT) \
-DRECEIVER_BATCHER_DEBUG_PRINT=$(RECEIVER_BATCHER_DEBUG_PRINT)

ifeq ($(OVERRIDE_RUNTIME), PureProcPerNUMA)
	CXXFLAGS += -DIS_PURE_PROC_PER_NUMA=1
else
	CXXFLAGS += -DIS_PURE_PROC_PER_NUMA=0
endif

ifeq ($(OVERRIDE_RUNTIME), PureProcPerNUMAMilan)
	CXXFLAGS += -DIS_PURE_PROC_PER_NUMA_MILAN=1
else
	CXXFLAGS += -DIS_PURE_PROC_PER_NUMA_MILAN=0
endif


ifeq ($(OS), nersc)
	ifeq ($(CORI_ARCH), )
$(error On Cori, CORI_ARCH must be defined in your environment.)
	endif
	CXXFLAGS += -DCORI_ARCH=$(CORI_ARCH)
endif

ifeq ($(COLLECT_THREAD_TIMELINE_DETAIL), 1)
ifdef TIMER_INTERVAL_NUM_GUESS
CXXFLAGS += -DTIMER_INTERVAL_NUM_GUESS=$(TIMER_INTERVAL_NUM_GUESS)
else
CXXFLAGS += -DTIMER_INTERVAL_NUM_GUESS=2000
endif
endif

CXXFLAGS += -DSHOW_SKIPPED_INTERVALS=$(SHOW_SKIPPED_INTERVALS)
CXXFLAGS += -DTRUNCATE_INTERVALS_OUTSIDE_ETE=$(TRUNCATE_INTERVALS_OUTSIDE_ETE)
PROCESS_CHANNEL_VERSION ?= -1 # set to a sentinel for MPI
PRINT_PROCESS_CHANNEL_STATS ?= 0
CXXFLAGS += -DPRINT_PROCESS_CHANNEL_STATS=$(PRINT_PROCESS_CHANNEL_STATS)
CXXFLAGS += -DMAX_BYTES_FOR_INTRANODE_QUEUE_STYLE=$(MAX_BYTES_FOR_INTRANODE_QUEUE_STYLE)

# Add in additional options for process channels 
# annoyingly, we have to explicitly pass in the PCV as an environment variable to the shell call.
PCV_VARS := $(shell PROCESS_CHANNEL_VERSION=$(PROCESS_CHANNEL_VERSION) ruby --disable=gems $(CPL)/support/Makefile_includes/determine_preprocessor_vars.rb)

#$(error --> $(PCV_VARS))
# TODO: implement error checking
# ifneq ($(.SHELLSTATUS),0)
#   $(error determine_preprocessor_vars failed. See output above. Returned $(.SHELLSTATUS))
# endif
CXXFLAGS += $(PCV_VARS)
# sets up NUMACTL_CPUS
include $(SUPPORT_PATH)/Makefile_includes/Makefile.cpu_config.mk

# sets up run configuration for all run types
include $(SUPPORT_PATH)/Makefile_includes/Makefile.run_config.mk
ifeq ($(MPI_ENABLED),1)
	# these are so the application can tell if MPI is running or not
	CFLAGS += -DMPI_ENABLED=1
	CXXFLAGS += -DMPI_ENABLED=1
else
	CFLAGS += -DMPI_ENABLED=0
	CXXFLAGS += -DMPI_ENABLED=0
endif

# There should be 5 different runtimes. Keep these up to date with Makefile.run_config.mk, Makefile.misc.mk, and BenchmarkHelpers.node_config
ifeq ($(RUNTIME),MPI)
	CFLAGS += -DRUNTIME_IS_MPI=1
	CXXFLAGS += -DRUNTIME_IS_MPI=1
endif
ifeq ($(RUNTIME),PureSingleThread)
	CFLAGS += -DRUNTIME_IS_PURE=1
	CXXFLAGS += -DRUNTIME_IS_PURE=1
endif
ifeq ($(RUNTIME),PureSingleThreadPerNode)
	CFLAGS += -DRUNTIME_IS_PURE=1
	CXXFLAGS += -DRUNTIME_IS_PURE=1
endif
ifeq ($(RUNTIME),Pure)
	CFLAGS += -DRUNTIME_IS_PURE=1
	CXXFLAGS += -DRUNTIME_IS_PURE=1
endif
ifeq ($(RUNTIME),PureProcPerNUMA)
	CFLAGS += -DRUNTIME_IS_PURE=1
	CXXFLAGS += -DRUNTIME_IS_PURE=1
endif

CFLAGS   += $(USER_CFLAGS)
CXXFLAGS += $(USER_CXXFLAGS)
LIBS     += $(USER_LIBS)
LFLAGS   += $(USER_LFLAGS)

CFLAGS += -DPURE_RANK_0_STATS_ONLY=$(PURE_RANK_0_STATS_ONLY)
CXXFLAGS += -DPURE_RANK_0_STATS_ONLY=$(PURE_RANK_0_STATS_ONLY)

# TODO(jim) this seems off; I think this should be --analyze to run clang's static analyzer. 
# These may be the gcc options. Clang's analyzer is probably better, although I'm not sure if it's fully superceded by clang-tidy.
# gcc on edison seems to require -Wall to enable attributes: http://www.unixwiz.net/techtips/gnu-c-attributes.html
CXXFLAGS_FOR_ANALYZE = -Wmost -Wextra -Weffc++ -Wall

TIDY_LOG = tidy.log

############################################################################
############################################################################
#                                                                          #
#  NO CFLAGS or CXXFLAGS changes below this point.                         #
#                                                                          #
############################################################################
############################################################################

# determine object file build dir depending on various command line attributes
# determine_objs_dir.rb assumes that all relevant variables have been exported to the environment by this point
# either at the command line or by the test makefile calling this one.
__OBJ_DIR_PARAMS = $(SUPPORT_PATH)/Makefile_includes/determine_objs_dir.rb --debug=$(DEBUG) \
--profile=$(PROFILE) --asan=$(ASAN) --tsan=$(TSAN) --msan=$(MSAN) --ubsan=$(UBSAN) --valgrind=$(VALGRIND_MODE) --thread-logger=$(ENABLE_THREAD_LOGGER) \
--trace-comm=$(TRACE_COMM) --trace-mpi-calls=$(TRACE_MPI_CALLS) --os=$(OS) --verbose=$(VERBOSE_OBJ_DIR) \
--prepend-hostname-and-extra-prefix=$(PREPEND_HOSTNAME_AND_EXTRA_PREFIX) --pcv=$(PROCESS_CHANNEL_VERSION) --print-pc-stats=$(PRINT_PROCESS_CHANNEL_STATS) \
--cflags='$(CFLAGS)' --cxxflags='$(CXXFLAGS)' --lflags='$(LFLAGS)' --libs='$(LIBS)' --collect-timeline=$(COLLECT_THREAD_TIMELINE_DETAIL) --debugging-verbose=$(OBJ_DIR_DEBUG_VERBOSE)
export OBJ_DIR_PREFIX ?= $(shell $(__OBJ_DIR_PARAMS))

# we want the first defintion of FLAGS_HASH (from the application-level Makefile) to persist and not be redefined by the library
export FLAGS_HASH ?= $(shell $(__OBJ_DIR_PARAMS) --flags-hash-only=1)

BUILD_PREFIX := $(BUILD_DIR)/$(OBJ_DIR_PREFIX)
# ifneq ($(.SHELLSTATUS),0)
#   $(error determine_objs_dir failed. See output above. Returned $(.SHELLSTATUS))
# endif

.PHONY: .verify_obj_dir .verify_pcv_vars

# the dot before the name allows this target to not be the default goal
.verify_obj_dir: 
ifeq ($(OBJ_DIR_PREFIX), )
	$(error "Object directory prefix (OBJ_DIR_PREFIX) is not set correctly, likely due to incompatible options. Check for error messages above.")
endif

# this will fail out if vars are not supported
.verify_pcv_vars:
	$(CPL)/support/Makefile_includes/determine_preprocessor_vars.rb

