# Author: James Psota
# File:   Makefile.sanitizer_opts.mk 

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

# TODO: consider turning off all debug checks when running in sanitizer mode. Or, run with and without debug checks fok full coverage.
# TODO: consider using llvmsymbolizer both on cag and osx.

ifeq ($(TSAN),1)
	TSAN_FLAGS += -fsanitize=thread -g -O0 -fno-inline -fno-omit-frame-pointer
	CFLAGS += $(TSAN_FLAGS)
    CXXFLAGS += $(TSAN_FLAGS)

	export TSAN_OUT_LOGFILE=tsan_out
	SAN_HALT_ABORT_ON_ERROR ?= 1
	export TSAN_OPTIONS=history_size=7:suppressions=$(CPL)/support/sanitizer/aggressive_supressions:log_path=$(SANITIZER_LOG_DIR)/$(TSAN_OUT_LOGFILE):halt_on_error=$(SAN_HALT_ABORT_ON_ERROR):abort_on_error=$(SAN_HALT_ABORT_ON_ERROR):verbosity=0:no_huge_pages_for_shadow=0
	# try later:  external_symbolizer_path=$(shell "which llvm-symbolizer")
		
	# NERSC-specific items. it unfortunately requires -dynamic because of the error message:
	#   "g++: error: cannot specify -static with -fsanitize=address". May work...
	ifeq ($(OS),nersc)
		CFLAGS += -dynamic
		CXXFLAGS += -dynamic
	endif

else ifeq ($(ASAN),1)
	# docs: http://clang.llvm.org/docs/AddressSanitizer.html
	SAN_HALT_ABORT_ON_ERROR ?= 1
	ASAN_FLAGS += -fsanitize=address -fPIE -fPIC -fno-inline -fno-omit-frame-pointer
	CFLAGS += $(ASAN_FLAGS)
	CXXFLAGS += $(ASAN_FLAGS)

	# options: https://github.com/google/sanitizers/wiki/AddressSanitizerFlags
	export ASAN_OUT_LOGFILE=asan_out
	export ASAN_OPTIONS=log_path=$(SANITIZER_LOG_DIR)/$(ASAN_OUT_LOGFILE):alloc_dealloc_mismatch=true:halt_on_error=$(SAN_HALT_ABORT_ON_ERROR):abort_on_error=$(SAN_HALT_ABORT_ON_ERROR):alloc_dealloc_mismatch=0

	# NERSC-specific items. it unfortunately requires -dynamic because of the error message:
	#   "g++: error: cannot specify -static with -fsanitize=address". May work...
ifeq ($(OS),nersc)
	ASAN_FLAGS += -dynamic
endif

else ifeq ($(MSAN),1)
	MSAN_FLAGS=-fsanitize=memory -fPIE -fPIC -g -O0 -fno-inline -fno-omit-frame-pointer -fno-optimize-sibling-calls
	CFLAGS += $(MSAN_FLAGS)
	CXXFLAGS += $(MSAN_FLAGS)
	export MSAN_OUT_LOGFILE = msan_out
	export MSAN_OPTIONS=log_path=$(SANITIZER_LOG_DIR)/$(MSAN_OUT_LOGFILE)

else ifeq ($(UBSAN),1)
	UBSAN_FLAGS=-fsanitize=undefined -fPIE -fPIC -g -O0 -fno-inline -fno-omit-frame-pointer -fno-optimize-sibling-calls
	CFLAGS += $(UBSAN_FLAGS)
	CXXFLAGS += $(UBSAN_FLAGS)
	UBSAN_OUT_LOGFILE = ubsan_out
	export UBSAN_OPTIONS=log_path=$(SANITIZER_LOG_DIR)/$(UBSAN_OUT_LOGFILE)
	
endif