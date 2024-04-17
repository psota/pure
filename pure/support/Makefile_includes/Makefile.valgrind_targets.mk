# Author: James Psota
# File:   Makefile.valgrind_targets.mk 

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

# this Makefile fragment includes targets related to valgrind

# variables required to be defined
#  EXTRA_DEBUGINFO_PATH
#  NPROCS
#  VALGRIND_LOG_DIR
#  MAIN

VALGRIND_LOG_DIR ?= "__PROTECTOR__OF__DIRECTORIES__"
VALGRIND_OPTS =
ifneq ($(EXTRA_DEBUGINFO_PATH),)
	VALGRIND_OPTS += "--extra-debuginfo-path=$(EXTRA_DEBUGINFO_PATH)"
endif

# special version of valgrind on lanka
# ** As of Sept. 2019, I couldn't find any machines that used AMD
# and trying out Intel version to debug something. May need to revert
# this back at some point.
#ifeq ($(IS_COMMIT_MACHINE),1)
#	VALGRIND_BIN = valgrind-amd
#else 
	VALGRIND_BIN = valgrind
#endif

verify_valgrind_opts:
ifeq ($(VALGRIND_LOG_DIR),)
	$(error Trying to run a valgrind tool, but VALGRIND_LOG_DIR is not set)
endif
ifeq ($(VALGRIND_LOG_DIR),"__PROTECTOR__OF__DIRECTORIES__")
	$(error Trying to run a valgrind tool, but VALGRIND_LOG_DIR is not set)
endif
ifeq ($(NPROCS),)
	$(error Trying to run a valgrind tool, but NPROCS is not set)
endif
ifeq ($(MAIN),)
	$(error Trying to run a valgrind tool, but MAIN is not set)
endif
ifeq ($(ASAN), 1)
	$(error Trying to run a valgrind tool, but ASAN is on and must be off)
endif
ifeq ($(MSAN), 1)
	$(error Trying to run a valgrind tool, but MSAN is on and must be off)
endif
ifeq ($(TSAN), 1)
	$(error Trying to run a valgrind tool, but TSAN is on and must be off)
endif
# ifneq ($(DEBUG), 1)
# 	$(error Trying to run a valgrind tool, but DEBUG is off and must be on)
# endif

valgrind-cleanup:
	[[ -e $(VALGRIND_LOG_DIR) ]] || mkdir $(VALGRIND_LOG_DIR)
	$(RM_F) $(VALGRIND_LOG_DIR)/*

valgrind: verify_valgrind_opts main-prebuild valgrind-cleanup
	$(warn Valgrind doesn't seem to be working with Pure. Seems to be choking on something related to threads, etc. Probably best to use ASAN on Linux for now.)
	$(RUN_DRIVER) $(VALGRIND_BIN) --trace-children=yes --log-file=$(VALGRIND_LOG_DIR)/memcheck-%p.valgrind --tool=memcheck --error-limit=no --leak-check=yes --dsymutil=yes $(VALGRIND_OPTS) $(APP_CMD)
# temporary hack for Phillipe reply. keeping here to show more advanced example.
#	$(MPIRUN) -np $(PURE_NUM_PROCS) valgrind -v -v -v -d -d -d --vgdb-stop-at=valgrindabexit --trace-children=yes --valgrind-stacksize=8388608 --log-file=$(VALGRIND_LOG_DIR)/memcheck-%p.valgrind --tool=memcheck --dsymutil=yes --extra-debuginfo-path=$(MAKEFILE_DIR)../lib $(MAIN) $(RUN_ARGS)
	wc -l $(VALGRIND_LOG_DIR)/*

valgrind-rawrun: verify_valgrind_opts valgrind-cleanup
	$(warn Valgrind doesn't seem to be working with Pure. Seems to be choking on something related to threads, etc. Probably best to use ASAN on Linux for now.)
	$(RUN_DRIVER) $(VALGRIND_BIN) --trace-children=yes --log-file=$(VALGRIND_LOG_DIR)/memcheck-%p.valgrind --tool=memcheck --leak-check=yes --dsymutil=yes $(VALGRIND_OPTS) $(APP_CMD)
# temporary hack for Phillipe reply. keeping here to show more advanced example.
#	$(MPIRUN) -np $(PURE_NUM_PROCS) valgrind -v -v -v -d -d -d --vgdb-stop-at=valgrindabexit --trace-children=yes --valgrind-stacksize=8388608 --log-file=$(VALGRIND_LOG_DIR)/memcheck-%p.valgrind --tool=memcheck --dsymutil=yes --extra-debuginfo-path=$(MAKEFILE_DIR)../lib $(MAIN) $(RUN_ARGS)
	wc -l $(VALGRIND_LOG_DIR)/*

# for some reason the --log-file is not being fully recognized, so we use the default output files, and manually move them around.
# old manual version: $(MPIRUN) -np $(NPROCS) valgrind -v --tool=massif $(VALGRIND_OPTS) --dsymutil=yes $(TEST_DIR)/$(MAIN) $(RUN_ARGS)
massif: valgrind-cleanup main-prebuild verify_valgrind_opts
	$(RM_F) massif.out.*
ifeq ($(USE_JEMALLOC), 1)
	@echo ******* WARNING WARNING jemalloc is on and it seems to create false positives with valgrind *******. Probably turn it off.
endif	
	$(RUN_DRIVER) $(VALGRIND_BIN) --tool=massif $(VALGRIND_OPTS) --dsymutil=yes $(APP_CMD)
	mv massif.out.* $(VALGRIND_LOG_DIR)

massif-stack: valgrind-cleanup main-prebuild verify_valgrind_opts
	$(RM_F) massif.out.*
	$(RUN_DRIVER) $(VALGRIND_BIN) --tool=massif --stacks=yes $(VALGRIND_OPTS) --dsymutil=yes $(APP_CMD)
	mv massif.out.* $(VALGRIND_LOG_DIR)

ms_print: massif_logs=$(shell ls $(VALGRIND_LOG_DIR)/massif.out.*)
ms_print: massif
	$(foreach massif_log,$(massif_logs),ms_print $(massif_log) > $(massif_log).ms;)

ms_print_stack: massif_logs=$(shell ls $(VALGRIND_LOG_DIR)/massif.out.*)
ms_print_stack: massif-stack
	$(foreach massif_log,$(massif_logs),ms_print $(massif_log) > $(massif_log).ms;)

ms_print_totals: ms_print
	$(SUPPORT_PATH)/misc/ms_print_totals.rb $(VALGRIND_LOG_DIR)/massif.out.*.ms

ms_print_totals_no_massif_run: massif_logs=$(shell ls $(VALGRIND_LOG_DIR)/massif.out.*)
ms_print_totals_no_massif_run:
	$(RM_F) $(VALGRIND_LOG_DIR)/massif.out.*.ms
	$(foreach massif_log,$(massif_logs),ms_print $(massif_log) > $(massif_log).ms;)
	$(SUPPORT_PATH)/misc/ms_print_totals.rb $(VALGRIND_LOG_DIR)/massif.out.*.ms

###### DEPRECATED ###########################
# valkyrie: verify_valgrind_opts main-prebuild valgrind-cleanup
# 	$(MPIRUN) -np $(NPROCS) $(VALGRIND_BIN) -q --trace-children=yes --log-file=$(VALGRIND_LOG_DIR)/memcheck-%p.valgrind --xml=yes --xml-file=$(VALGRIND_LOG_DIR)/memcheck-%p.valgrind.xml --child-silent-after-fork=yes --tool=memcheck --leak-check=yes --dsymutil=yes $(VALGRIND_OPTS) $(TEST_DIR)/$(MAIN) $(RUN_ARGS)
# 	$(SUPPORT_PATH)/misc/valkyrie_select_input.rb $(NPROCS)

# helgrind-deprecated: verify_valgrind_opts check-valgrind-thread-config $(MAIN)
# 	$(error helgrind is deprecated. You probably want to be using TSAN.)
# 	$(warn DEBUG must be set to true to run with Valgrind. It is currently $(DEBUG))
# 	$(MPIRUN) -np $(NPROCS) $(VALGRIND_BIN) -v --log-file=$(VALGRIND_LOG_DIR)/helgrind-%p.valgrind --tool=helgrind $(VALGRIND_OPTS) --dsymutil=yes $(TEST_DIR)/$(MAIN) $(RUN_ARGS)
