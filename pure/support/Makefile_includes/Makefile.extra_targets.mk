# Author: James Psota
# File:   Makefile.extra_targets.mk 

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

# see http://robcottingham.ca/2008/03/os-x-applications-constantly-asking-permission-to-accept-incoming-connections-heres-a-fix/ s# getting warnings to "Allow incoming connections". You have to point directly to the binary in the proper place. May have to
# delete the entry and re-add it.

# Set up GDB script
define BASE_GDB_COMMANDS
# default logs will go to gdb.txt
set logging on
show logging
set breakpoint pending on
set pagination off
# enable reversable debugging on multithreaded programs. turn this into an option if it makes gdb too slow.
# need to start record only after program started running
# record
# these seem to be unnecessary?
#catch signal SIGABRT
#break __assert_fail
endef

ifeq ($(TSAN),1)
TSAN_GDB_COMMANDS = set disable-randomization off
endif
ifeq ($(ASAN),1)
ASAN_GDB_COMMANDS = 
endif

define GDB_COMMANDS
$(BASE_GDB_COMMANDS)
$(TSAN_GDB_COMMANDS)
$(ASAN_GDB_COMMANDS)
$(USER_GDB_COMMANDS)
run $(RUN_ARGS)
endef
# http://stackoverflow.com/questions/649246/is-it-possible-to-create-a-multi-line-string-variable-in-a-makefile
# export this so it can later be referenced as an environment variable (bash), not just a Makefile variable.
export GDB_COMMANDS

RUN_OUTPUT_LOG_DIR ?= logs
RUN_OUTPUT_FILENAME ?= $(RUN_OUTPUT_LOG_DIR)/all_output.log

SANITIZER_LOG_DIR ?= sanitizer
VALGRIND_LOG_DIR ?= valgrind
MPIP_LOG_DIR ?= mpip

# For the prebuild makefile, make will fail if it can't find the makefile
app_prebuild:
ifneq ($(APP_PREBUILD_MAKEFILE),)
	@make -f $(APP_PREBUILD_MAKEFILE)
endif
	$(shell [ -d mpi_debug_logs ] || mkdir -p "mpi_debug_logs" )
	$(shell [ -d $(RUN_OUTPUT_LOG_DIR) ] || mkdir $(RUN_OUTPUT_LOG_DIR))
	$(shell [ -d $(SANITIZER_LOG_DIR) ] || mkdir $(SANITIZER_LOG_DIR))
	$(shell [ -d $(VALGRIND_LOG_DIR) ] || mkdir $(VALGRIND_LOG_DIR))
	$(shell [ -d $(MPIP_LOG_DIR) ] || mkdir $(MPIP_LOG_DIR))
ifneq ($(MPI_HOSTFILE),)
	@echo "Generating proper hostfile for slurm. Should only be done on lanka or other supported hosts where this is necessary."
	$$CPL/support/misc/mpi_hostfile_for_slurm.rb
	@echo
	@echo "MPI NODES FOR THIS RUN::::::::::::::::::::::::::::::::::::::::::::::::::::::::"
	cat $(MPI_HOSTFILE)
endif
ifeq ($(COLLECT_THREAD_TIMELINE_DETAIL),1)
	$(warning ************************************************************************************************)
	$(RM_F) timer_interval/*
endif

# Configure thread automapper system
#
# if ENABLE_THREAD_AFFINITY_AUTOMAPPER is set to 1, the build system will automatically look for a file named
# THREAD_AFFINITY_AUTOMAPPER_FILENAME. If it's found, it uses it. If it's not found, it throws an error.
# This allows other scripts to just set ENABLE_THREAD_AFFINITY_AUTOMAPPER and have the filename be automatically
# determined by the following defintion.

# Only consider using the AUTOMAPPER if there's more than one MPI process
ifneq ($(PURE_NUM_PROCS),1) 
ifeq ($(ENABLE_THREAD_AFFINITY_AUTOMAPPER),1)
ifneq ("$(wildcard $(THREAD_AFFINITY_AUTOMAPPER_FILENAME))","")
THREAD_MAP_FILENAME := $(THREAD_AFFINITY_AUTOMAPPER_FILENAME)
else
$(error Unable to find THREAD_AFFINITY_AUTOMAPPER_FILENAME $(THREAD_AFFINITY_AUTOMAPPER_FILENAME). You should create one using $B/dt/comm_trace_driver.rb or a similar script))
endif   
endif   
endif   
export THREAD_MAP_FILENAME

app_postsuccess:
	@echo "████ SUCCESS (exit code 0) █████████████████████████████████████████████████████████"
	@printf "     End-to-end runtime: "
	$(CPL)/support/misc/longest_elapsed_time_from_json.rb $(TOTAL_THREADS) $(JSON_STATS_DIR)
	@echo
	@printf "...from JSON_STATS_DIR $(JSON_STATS_DIR), NUMA_SCHEME was $(NUMA_SCHEME). Also see rank_to_cpu_map.csv for the rank-to-cpu mapping used.\n\n"
	@echo "See the ranks labeled on the topology with:  make ranks-on-topo  (on Cori, run this on a login node)"

# DANGEROUS! Only use if you know what you are doing in the context of batch scripts, etc.
raw-background-run:
ifneq ($(BATCH_DRIVER_RUN_DIR),)
	@echo "Changing directory to $(BATCH_DRIVER_RUN_DIR)"
	cd $(BATCH_DRIVER_RUN_DIR) && 	JSON_STATS_DIR=$(JSON_STATS_DIR) $(RUN_CMD)
else
	JSON_STATS_DIR=$(JSON_STATS_DIR) $(RUN_CMD)
endif

# shortcut
rawrun: raw-background-run

rawrunstats: raw-background-run
	$(MAKE) app_postsuccess

#codesign_app: CODESIGN_RET = $(shell codesign -vvv -d $(BIN_NAME) 2>&1 2> /dev/null; echo $$?)
codesign_app:
ifeq ($(OS), osx)
	codesign -f -s JP_Code_Signer $(BIN_NAME) --deep
	@echo Codesigned application $(BIN_NAME).
endif

run-terms: main-prebuild
	$(MPIRUN) -np $(NPROCS) xterm -e "$(BIN_NAME) $(RUN_ARGS) ; sleep 100"
	make process-mpi-logs

gdb-setup: main-prebuild
	$(RM_F) gdb.txt
	@echo creating gdb command file $(GDB_SCRIPT)
	$(RM_F) $(GDB_SCRIPT)
	touch $(GDB_SCRIPT)

gdb: CXXFLAGS += -g3
gdb: -DGDB_BREAKPOINT=
gdb: gdb-setup
	@echo "gdb configured for OSX; opening one terminal per MPI process."
	@echo Warning: Not processing MPI logs
ifdef NPROCS
	$(MPIRUN) -np $(NPROCS) xterm -hold -e "gdb $(GDB_OPTS) -x $(GDB_SCRIPT) $(BIN_NAME) ; sleep 100"
else
	@echo "Warning: Running directly (no MPIRUN) as NPROCS is not defined"
	gdb $(GDB_OPTS) -x $(GDB_SCRIPT) $(BIN_NAME)
endif

gdb-raw: CXXFLAGS += -g3
gdb-raw: -DGDB_BREAKPOINT=
gdb-raw: gdb-setup
ifdef NPROCS
	@echo "WARNING: running all processes within one terminal. WARNING: no script passed."
	gdb $(MPIRUN) -np $(NPROCS)  $(GDB_OPTS) $(BIN_NAME)
else
	@echo "Warning: Running directly (no MPIRUN) as NPROCS is not defined"
	gdb $(GDB_OPTS) -x $(GDB_SCRIPT) $(BIN_NAME)
endif

# note: may have to make clean to get debugging symbols in place.
gdb-run: CXXFLAGS += -g3
gdb-run: -DGDB_BREAKPOINT=
gdb-run: gdb-setup
	echo "$$GDB_COMMANDS" >> $(GDB_SCRIPT)
	@echo $(GDB_SCRIPT) contains:
	cat $(GDB_SCRIPT)
# special case for a single process -- don't open xterms
ifeq ($(NPROCS),1)
	@echo "NPROCS is 1, so not opening xterms; running gdb directly in terminal."
	gdb $(GDB_OPTS) -x $(GDB_SCRIPT) $(BIN_NAME)
else 
	@# RUNNING MPI
ifeq ($(OS),nersc)
	srun $(SLURM_NODE_OPTIONS) --partition=debug -t 00:10:00 xterm -e "gdb $(GDB_OPTS) -x $(GDB_SCRIPT) $(BIN_NAME) ; sleep 100"
else
	@# NOT NERSC
	$(MPIRUN) -np $(NPROCS) xterm -e "gdb $(GDB_OPTS) -x $(GDB_SCRIPT) $(BIN_NAME) ; sleep 100"
endif
	@# ENDS RUNNING MPI
endif

lldb: main-prebuild
	@echo "TIP: Add extra LLDB commands to the LLDB_COMMANDS environment variable"
	echo "$(LLDB_COMMANDS)" > lldb.commands
	@echo "run" >> lldb.commands
ifeq ($(NPROCS), 1)
	#lldb -s lldb.commands $(BIN_NAME) -- $(RUN_ARGS)
	lldb $(BIN_NAME) -- $(RUN_ARGS)
else
	$(MPIRUN) -np $(NPROCS) xterm -e "lldb -s lldb.commands -- $(APP_CMD)"
endif

# Rewrite. variables are already set at this point!!!
sanitize-all: ensure_linux_machine
	$(RM_F) $(SANITIZER_LOG_DIR)/*
	@echo "Run the following command to run all sanitizers:\n"
	@echo
	@echo   $(SUPPORT_PATH)/misc/sanitize-all.sh
	@echo

ddt-howto: 
	@echo
	@echo "Before running this, make sure you "
	@echo "     module load allinea-forge &&  make clean && make " 
	@echo
	@echo "go to an interactive node and  "
	@echo
	@echo  "    module load allinea-forge && make ddt"
	@echo

ddt: ensure_nersc_slurm
	@echo "Before running this, make sure you 'module load allinea-forge' and then make clean and go to an interactive node"
	@echo "Also make sure to open Allinea Forge on OSX and connect to Cori."
	@echo "Also make sure the binary is already compiled from a login node for speed."
	@echo
	ddt --connect $(RUN_CMD)

# tried to run lanka with exclusive mode and this specificed queue, but got error: "srun: error: Unable to create job step: Requested node configuration is not available" even though allocation via alloc succeeded.
#	salloc $(SLURM_NODE_OPTIONS) --exclusive -p lanka-v3
salloc: SLURM_QUEUE ?= debug
salloc: SLURM_TIME ?= 00:30:00
salloc:
	@echo "allocating debug queue. pass environment variable SLURM_QUEUE to this target to override. e.g., SLURM_QUEUE=release. Override time request with SLURM_TIME."
ifeq ($(OS),nersc)
	@echo "Running with NERSC configuration"
	salloc $(SLURM_NODE_OPTIONS) --partition=$(SLURM_QUEUE) -t $(SLURM_TIME)
else
	@echo "Running with lanka (cag) configuration with exclusive mode on partition lanka-v3"
	salloc $(SLURM_NODE_OPTIONS)
endif

verify_root: curr_dir = $(shell pwd)
verify_root:
ifneq ($(shell whoami),root)
	$(error "You must become root to run this target.  sudo bash   and then cd ${curr_dir} ")
endif

verify_not_root:
ifeq ($(shell whoami),root)
	$(error "You must not be root to run this target.  sudo bash   and then cd ${curr_dir} ")
endif

numactl_status:
ifeq ($(NUMA_SCHEME),)
		@echo Error Unspecified NUMA_SCHEME; please specify none, hyperthread_siblings, shared_l3, different_numa
		exit
else
ifeq ($(OS),$(filter $(OS),cag nersc))
		@echo "$(NUMACTL_DESC): using the following CPUs: $(NUMACTL_CPUS)"
else
		@echo "WARNING: using NUMACTL_CPUS only works on CAG and NERSC machines; ignoring option."
endif
endif

# via http://www.commandlinefu.com/commands/view/3584/remove-color-codes-special-characters-with-sed
remove_ansi_codes:
	@echo "Removing ANSI codes for file $(FILE_TO_REMOVE_ANSI_FROM)"
	sed "s/\x1B\[([0-9]{1,2}(;[0-9]{1,2})?)?[m|K]//g" $(FILE_TO_REMOVE_ANSI_FROM) > $(FILE_TO_REMOVE_ANSI_FROM).txt

clang-format:
	clang-format -i *.cpp
	clang-format -i *.c
	clang-format -i *.h 
	@echo "Done formatting cpp, c, and .h files. Tip: commit now."

# temporary target for Excel file to keep track of performance optimizations
experiment_details_header:
	@printf "date\toptimization\tos\truntime\tconfig_unique_id\t"
	@printf "run_num_for_config\tprocs\tthreads\tomp_num_threads\ttotal_threads\tprocess_channel_version\tpc_buffered_msg_size\t"
	@printf "use_jemalloc\tuse_asmlib\t"
	@printf "run_args\t"
	@printf "git_hash\tbranch\ttest_dir\tbin_name\tnumactl_desc\tnumactl_cpus\ttopology\tclass\tcxxflags\tcflags\t"
	@printf "run_args_0\trun_args_1\trun_args_2\trun_args_3\trun_args_4\trun_args_5\trun_args_6\trun_args_7\trun_args_8\trun_args_9\trun_args_10\trun_args_11\trun_args_12\trun_args_13\trun_args_14\trun_args_15\trun_args_16\trun_args_17\trun_args_18\trun_args_19\t"
	@printf "run_args_20\trun_args_21\trun_args_22\trun_args_23\trun_args_24\trun_args_25\trun_args_26\trun_args_27\trun_args_28\trun_args_29\trun_args_30\trun_args_31\trun_args_32\trun_args_33\trun_args_34\trun_args_35\trun_args_36\trun_args_37\trun_args_38\trun_args_39\t"
	@printf "heap_max_total_bytes\t"
	@printf "extra_description\t"

	@printf "pure_thread_rank\tpure_run_unique_id\thostname\tstats_filename\t"

	@printf "elapsed_cycles\t"
	@ruby $(CPL)/support/misc/custom_timer_headers.rb

	@make -f $$CPL/support/Makefile_includes/Makefile.profiling.mk --no-print-directory profile-stat-tsv-header
	@printf "\n"

# IMPORTANT: Keep number of run_args in header row in line with --max-run-args in experiment_details target (0-indexed)
# make sure the arguments use hyphens, not underscores
experiment_details:
	@$(SUPPORT_PATH)/misc/experiment_details.rb \
	--os=$(OS) \
	--runtime-system=$(RUNTIME) \
	--nprocs=$(NPROCS) \
	--threads=$(PURE_RT_NUM_THREADS) \
	--omp-num-threads=$(OMP_NUM_THREADS) \
	--total-threads=$(TOTAL_THREADS) \
	--config-unique-id="$(CONFIG_UNIQUE_ID)" \
	--process-channel-version="$(PROCESS_CHANNEL_VERSION)" \
	--process-channel-buffered-msg-size="$(PROCESS_CHANNEL_BUFFERED_MSG_SIZE)" \
	--use-jemalloc="$(USE_JEMALLOC)" \
	--use-asmlib="$(USE_ASMLIB)" \
	--git-hash=$(shell git rev-parse HEAD) --git-branch=$(shell git rev-parse --abbrev-ref HEAD) \
	--test-dir=$(shell pwd) --bin-name=$(BIN_NAME) \
	--run-args="$(RUN_ARGS)" \
	--max-run-args=40 \
	--heap-max-total-bytes="$(HEAP_MAX_TOTAL_BYTES)" \
	--extra-description="$(EXTRA_DESCRIPTION)" \
	--run-num-for-config="$(RUN_NUM_FOR_CONFIG)" \
	--numactl-desc="$(NUMACTL_DESC)" \
	--numactl-cpus="$(NUMACTL_CPUS)" \
	--topology="$(TOPO)" \
	--class="$(CLASS)" \
	--cxxflags="$(CXXFLAGS)" \
	--cflags="$(CFLAGS)" \
	--json-stats-dir="$(JSON_STATS_DIR)" \
	--perf-stats-file=$(PERF_STAT_OUTFILE) \
	--rank-0-stats-only=$(PURE_RANK_0_STATS_ONLY)

# # deprecated
# experiment_details_command:
# 	@echo @$(SUPPORT_PATH)/misc/experiment_details.rb \
# 	--os=$(OS) \
# 	--runtime-system=$(RUNTIME) \
# 	--nprocs=$(NPROCS) \
# 	--threads=$(PURE_RT_NUM_THREADS) \
# 	--total-threads=$(TOTAL_THREADS) \
# 	--config-unique-id=$(CONFIG_UNIQUE_ID) \
# 	--process-channel-version="$(PROCESS_CHANNEL_VERSION)" \
# 	--process-channel-buffered-msg-size="$(PROCESS_CHANNEL_BUFFERED_MSG_SIZE)" \
# 	--git-hash=$(shell git rev-parse HEAD) \
# 	--git-branch=$(shell git rev-parse --abbrev-ref HEAD) \
# 	--test-dir=$(shell pwd) --bin-name=$(BIN_NAME) \
# 	--run-args="$(RUN_ARGS)" \
# 	--max-run-args=16 \
# 	--heap-max-total-bytes="$(HEAP_MAX_TOTAL_BYTES)" \
# 	--extra-description="$(EXTRA_DESCRIPTION)" \
# 	--run-num-for-config="$(RUN_NUM_FOR_CONFIG)" \
# 	--numactl-desc="$(NUMACTL_DESC)" \
# 	--numactl-cpus="$(NUMACTL_CPUS)" \
# 	--topology="$(TOPO)" \
# 	--class="$(CLASS)" \
# 	--cxxflags="$(CXXFLAGS)" \
# 	--cflags="$(CFLAGS)" \
# 	--json-stats-dir="$(JSON_STATS_DIR)" \
# 	--perf-stats-file=$(PERF_STAT_OUTFILE)

# it will fail if the binary is not there
stat_bin:
	stat $(BIN_NAME)

ensure_profile_mode:
ifneq ($(PROFILE), 1)
	$(error ERROR: You must run this command in profile mode)
endif

ensure_map_profile_mode:
ifneq ($(MAP_PROFILE), 1)
	$(error ERROR: You must run this command in map profile mode (i.e., set MAP_PROFILE to 1))
endif

ensure_cag_machine:
ifneq ($(OS), cag)
	$(error ERROR: You must run this command on a CAG machine)
endif

ensure_osx_machine:
ifneq ($(OS), osx)
	$(error ERROR: You must run this command on the OSX laptop)
endif

ensure_linux_machine:
ifneq ($(OS), $(filter $(OS), cag nersc))
	$(error ERROR: You must run this command on a Linux machine)
endif

ensure_commit_machine: 
ifneq ($(IS_COMMIT_MACHINE),1)
	$(error ERROR: You must run this command on a Commit machine, defined by IS_COMMIT_MACHINE in the environment)
endif

ensure_commit_slurm: ensure_commit_machine
ifeq ($(shell env | grep SLURM_NODELIST),)
	$(error ERROR: You must run this command on a Commit machine on a SLURM allocation. So, use salloc or srun)
endif

ensure_nersc_slurm:
ifneq ($(OS), nersc)
	$(error ERROR: target must be run on a NERSC machine such as Cori)
endif
ifeq ($(SLURM_JOB_NODELIST),)
	$(error ERROR: target must be run within a slurm session (interactive or batch))
endif

ensure_intel_tools:
ifeq ($(shell $(CC) --version | grep Intel), )
	$(error ERROR: you must run intel tools to run this target. Run bash function swap_to_intel and make clean)
endif


# work in progress
loc: ensure_osx_machine
	cloc $(CPL)/src $(CPL)/include/pure $(CPL)/support --fullpath --not-match-d="src\/3rd_party|include\/pure\/3rd_party|support\/experiments\/benchmarks\/.*\/runs\/*|support\/experiments\/benchmarks\/.*\.json|pure\/src\/\.d" --found=cloc.txt

filename_from_opts:
	$(SUPPORT_PATH)/misc/filename_from_opts.rb

# example: make print-OBJS
# https://blog.melski.net/2010/11/30/makefile-hacks-print-the-value-of-any-variable/
print-%:
	@echo '$*=$($*)'
	@echo '  origin = $(origin $*)'
	@echo '  flavor = $(flavor $*)'
	@echo '   value = $(value  $*)'
