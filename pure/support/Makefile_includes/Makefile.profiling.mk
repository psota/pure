# Author: James Psota
# File:   Makefile.profiling.mk 

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

PERF_REPORT_CMD = ocperf.py report -n --show-nr-samples --percentage=relative

REPORT_FIELDS = 
ifeq ($(NUMA_SCHEME), none)
	# don't add cpu if migrations are allowed
	EXTRA_REPORT_FIELDS ?= 
else
	EXTRA_REPORT_FIELDS ?= ,cpu
endif

ifeq ($(NPROCS),1)
	# Pure -- differentiate based on thread id
	PERF_REPORT_CMD += --sort=overhead,comm,symbol,dso$(EXTRA_REPORT_FIELDS)
else
	PERF_REPORT_CMD += --sort=overhead,symbol,dso,pid$(EXTRA_REPORT_FIELDS)
endif

PERF_STAT_EVENTS ?= cycles:pp, instructions, branches, branch-misses, L1-dcache-loads, L1-dcache-load-misses, LLC-loads, LLC-load-misses, offcore_response.all_data_rd.llc_hit.hitm_other_core

# qpi_data_bandwidth_tx could be useful to measure cross-NUMA node bandwidth

# TODO: try these out:
# perf c2c (Linux 4.10+): cache-2-cache and cacheline false sharing analysis.
  # see https://joemario.github.io/blog/2016/09/01/c2c-blog/
# perf lock: lock analysis.
# perf mem: memory access analysis.

#DEFAULT_PERF_EVENTS = cycles:pp,L1-dcache-load-misses,LLC-load-misses,context-switches,cpu-migrations,offcore_response.all_data_rd.llc_hit.hitm_other_core,offcore_response.all_data_rd.llc_miss.remote_hitmoffcore_response.all_data_rd.llc_miss.remote_hit_forward,offcore_response.all_rfo.llc_miss.remote_hit_forward
#DEFAULT_PERF_EVENTS = cycles:pp,L1-dcache-load-misses,LLC-load-misses,context-switches,cpu-migrations,offcore_response.all_data_rd.llc_hit.hitm_other_core,offcore_response.all_data_rd.llc_miss.remote_hitm,offcore_response.all_data_rd.llc_miss.remote_hit_forward,offcore_response.all_rfo.llc_miss.remote_hit_forward
#DEFAULT_PERF_EVENTS = cycles:pp,L1-dcache-load-misses,LLC-load-misses,context-switches,cpu-migrations,offcore_response.all_data_rd.llc_hit.hitm_other_core,offcore_response.demand_data_rd.llc_miss.remote_hitm
#DEFAULT_PERF_EVENTS = cycles,cycles:pp
#DEFAULT_PERF_EVENTS = cycles:pp,L1-dcache-load-misses,LLC-load-misses,context-switches,cpu-migrations,offcore_response.all_data_rd.llc_hit.hitm_other_core,offcore_response.all_data_rd.llc_miss.remote_hitmoffcore_response.all_data_rd.llc_miss.remote_hit_forward,offcore_response.all_rfo.llc_miss.remote_hit_forward
# to change this real time, just redefine PERF_EVENTS to whatever you want and re-run "make profile"
# export PERF_EVENTS="cache-misses,L1-dcache-load-misses,L1-dcache-store-misses,LLC-load-misses,LLC-store-misses,offcore_response.all_data_rd.llc_hit.hitm_other_core"
DEFAULT_PERF_EVENTS = cycles:ppp
CAG_UID = 14499
WEBSITE_URL = http://people.csail.mit.edu/jim/pure/flamegraphs

verify_profile_opts:
ifneq ($(OS), $(filter $(OS), cag nersc))
	$(error perf_events, pcm, etc. tools can only be run on linux)
endif
ifneq ($(PROFILE), 1)
	$(warning **********************************************************)
	$(warning **********************************************************)
	$(warning **********************************************************)
	$(error Trying to run a profiler tool, but PROFILE must be set to 1)
	$(warning **********************************************************)
	$(warning **********************************************************)
	$(warning **********************************************************)
endif
ifneq ($(DEBUG), 0)
	$(warning **********************************************************)
	$(error Trying to run a profiler tool, but DEBUG is on and must be off)
	$(warning **********************************************************)
endif
ifneq ($(OS), nersc)
# this option isn't supported on the old perf that's on Cori
EXTRA_PERF_RECORD_OPTS += --sample-cpu
endif

warn_profile_opts:
ifneq ($(OS), $(filter $(OS), cag nersc))
	$(error perf_events, pcm, etc. tools can only be run on linux)
endif
ifneq ($(PROFILE), 1)
	$(warning **********************************************************)
	$(warning **********************************************************)
	$(warning **********************************************************)
	$(warning WARNING: Trying to run a profiler tool, but PROFILE should be set to 1 for most represenative profile results.)
	$(warning **********************************************************)
	$(warning **********************************************************)
	$(warning **********************************************************)
endif

base_profile: verify_not_root verify_profile_opts main-prebuild numactl_status
	@echo "████████████████████████████████████████████████████████████████████████████████████"
ifeq ($(MPI_ENABLED), 1)
	PERF_CMD="ocperf.py record --stat --delay=$(DELAY_MSEC) -e $(PERF_EVENTS) $(EXTRA_PERF_RECORD_OPTS) $(PERF_CPU_OPT)" \
	APP_CMD="$(APP_CMD)" PERF_REPORT_CMD="$(PERF_REPORT_CMD)" $(RUN_DRIVER) $(CPL)/support/misc/mpi_perf_app.sh
else
	ocperf.py record --stat --delay=$(DELAY_MSEC) -e $(PERF_EVENTS) $(EXTRA_PERF_RECORD_OPTS) $(PERF_CPU_OPT) -- $(RUN_CMD)
endif
	@echo "████ SUCCESS (exit code 0) █████████████████████████████████████████████████████████"

profile: PERF_EVENTS ?= $(DEFAULT_PERF_EVENTS)
profile: DELAY_MSEC ?= 0
profile: base_profile
	$(PERF_REPORT_CMD)

profile-call-graph: EXTRA_PERF_RECORD_OPTS += -g
profile-call-graph: PERF_EVENTS ?= $(DEFAULT_PERF_EVENTS)
profile-call-graph: DELAY_MSEC ?= 500
profile-call-graph: base_profile
	$(PERF_REPORT_CMD)


profile-c2c: verify_not_root verify_profile_opts main-prebuild numactl_status
	@echo "ERROR: seems to only work on more modern linuxes (4+)"
	exit
	@echo "████████████████████████████████████████████████████████████████████████████████████"
ifdef NPROCS
	perf c2c record -- $(RUN_CMD)
else
	@echo "Warning: C2C profiling directly (no MPIRUN) as NPROCS is not defined"
	perf c2c record -- $(RUN_CMD)
endif
	@echo "████ SUCCESS (exit code 0) █████████████████████████████████████████████████████████"
	perf c2c report -NN -c pid,iaddr --full-symbols
	@# record and report with call graph info
	@# perf c2c record --call-graph dwarf,8192 -F 60000 -a --all-user sleep 5
	@# perf c2c report -NN -g --call-graph -c pid,iaddr --stdio 

profile-local-mpi: PERF_EVENTS ?= $(DEFAULT_PERF_EVENTS)
profile-local-mpi: verify_not_root verify_profile_opts
	@echo "In order to run this, you must ALREADY be running the program (via make run)."
	@echo "Make sure to run this from the same directory as the MPI application you want to analyze."
	@echo "WARNING: only profiles MPI processes running on this node ($(shell hostname))"
	$(CPL)/support/misc/profile_running_job.rb $(BIN_NAME_NO_PATH) $(NPROCS) '$(PERF_EVENTS)' $(TIMESTAMP)

profile-record: PERF_EVENTS ?= $(DEFAULT_PERF_EVENTS)
profile-record: DELAY_MSEC ?= 0
profile-record: base_profile

profile-report:
	$(PERF_REPORT_CMD)

report:
	$(MAKE) profile-report

# convert CSV events to tab-separated
profile-stat-tsv-header:
	@echo $(PERF_STAT_EVENTS) | tr ',' '\t'

profile-stat-csv: PERF_STAT_OPTS = "-x \;"
profile-stat-csv: PERF_STAT_OUTFILE ?= perf_stats.csv
profile-stat-csv: profile-stat-base
	# remove first two lines
	tail -n +3 $(PERF_STAT_OUTFILE) > $(PERF_STAT_OUTFILE).tmp && mv $(PERF_STAT_OUTFILE).tmp $(PERF_STAT_OUTFILE) 

profile-stat: PERF_STAT_OPTS =
profile-stat: PERF_STAT_OUTFILE ?= perf_stats.txt
profile-stat: profile-stat-base

profile-stat-base: 
profile-stat-base: verify_not_root verify_profile_opts main-prebuild numactl_status
	@echo "████████████████████████████████████████████████████████████████████████████████████"
	ocperf.py stat -r2  --big-num -e"$(PERF_STAT_EVENTS)" $(PERF_STAT_OPTS) --output $(PERF_STAT_OUTFILE) -- $(RUN_CMD)
	@echo "████ SUCCESS (exit code 0 - see $(PERF_STAT_OUTFILE)) █████████████████████████████████████████████████████████"

profile-top: PERF_EVENTS = $(DEFAULT_PERF_EVENTS)
profile-top: verify_profile_opts verify_not_root
	@echo "running perf top on these events: $(PERF_EVENTS)"
	ocperf.py top -g -e $(PERF_EVENTS) 

###########################################################################

flamegraph: PERF_OUT = out.perf
flamegraph: FOLDED_PERF_OUT = out.perf.folded
flamegraph: FLAME_DIR = flamegraphs
flamegraph: PERF_EVENTS = cycles:pp
flamegraph: FLAME_OUT_FILE ?= `EXTENSION='.svg' $(SUPPORT_PATH)/misc/filename_from_opts.rb $(PERF_EVENTS)`
flamegraph: FLAME_OUT ?= $(FLAME_DIR)/$(FLAME_OUT_FILE)
flamegraph: FLAMEGRAPH_WIDTH ?= 1900
flamegraph: EXTRA_PERF_RECORD_OPTS += "-g"
flamegraph: DELAY_MSEC ?= 500
flamegraph: base_profile
	@echo "Building flamegraph for event $(PERF_EVENTS)..."
	@mkdir -p $(FLAME_DIR)
	perf script $(PERF_CPU_OPT) > $(PERF_OUT)
	stackcollapse-perf.pl $(PERF_OUT) > $(FOLDED_PERF_OUT)
	flamegraph.pl --title="$(PERF_EVENTS) $(NUMACTL_DESC) $(FLAGS_HASH)" \
				  --subtitle="$(RUN_ARGS), $(NUMA_SCHEME), $(RUNTIME) v$(PROCESS_CHANNEL_VERSION)/$(PROCESS_CHANNEL_BUFFERED_MSG_SIZE))" \
	              --fonttype=Avenir --fontsize=11 --hash --cp $(FOLDED_PERF_OUT) --width=$(FLAMEGRAPH_WIDTH) $(FLAMEGRAPH_OPTS) > $(FLAME_OUT)
	cp $(FLAME_OUT) $(TEMP_WEB_DIR)
	@echo "Flamegraph of $(PERF_EVENTS) stored at $(FLAME_OUT)"
	@echo
	@echo
	@echo "$(WEBSITE_URL)/$(FLAME_OUT_FILE)"
	@echo

flame: flamegraph

heaptrack: ensure_cag_machine main-prebuild
	mkdir -p heaptrack
	@echo "Clearing out old heaptrack"
	$(RM_F) heaptrack/*
	$(RUN_DRIVER) heaptrack $(APP_CMD) 
	mv heaptrack*.gz heaptrack
	@echo "TIP: run pstree -p   while the program is running to map a particular process (rank) to the corresponding heaptrack parent process (and corresponding output file)."

heaptrack-analyze: ensure_cag_machine
ifndef HEAPFILE
	$(error The HEAPFILE environment var must be defined to call this targe. Something like this:  HEAPFILE=heaptract/foo.gz make heaptrack-analyze)
endif
	@echo "Analyzing heapfile $(HEAPFILE). This may take a while..."
	heaptrack -a $(HEAPFILE) > $(HEAPFILE).txt
	heaptrack_print --flamegraph-cost-type=peak --print-flamegraph=$(shell dirname $(HEAPFILE))/heapstats.txt -f $(HEAPFILE)
	flamegraph.pl --title "heaptrack: peak mem usage" --colors mem --countname "bytes (peak)" --fonttype=Avenir --fontsize=11 --hash --cp --width=1900 < $(shell dirname $(HEAPFILE))/heapstats.txt > $(HEAPFILE).svg
	mkdir -p $(TEMP_WEB_DIR)/$(shell dirname $(HEAPFILE))
	cp $(HEAPFILE).svg $(TEMP_WEB_DIR)/$(shell dirname $(HEAPFILE))
	@echo
	@echo "See $(WEBSITE_URL)/$(HEAPFILE).svg and $(HEAPFILE).txt (look for 'PEAK MEMORY CONSUMERS')"
	@echo

# https://docs.nersc.gov/programming/performance-debugging-tools/map/
$(TEST_DIR)/allinea-profiler.ld:
	make-profiler-libraries --lib-type=static

map: ensure_nersc_slurm ensure_profile_mode ensure_map_profile_mode $(TEST_DIR)/allinea-profiler.ld main-prebuild 
	map --connect $(RUN_CMD)

mapx: ensure_nersc_slurm ensure_profile_mode ensure_map_profile_mode $(TEST_DIR)/allinea-profiler.ld main-prebuild
	map $(APP_CMD)

###########################################################################

# INTEL TOOLS

#   Instructions for running VTune on Cori
#   1. cori
#   2. interactive session
#   3. swap_to_intel command to use intel toolchain
#   4. make clean in case anything was built with gcc
#   5. make vtune
#   6. get files to local computer -- todo

# testing with no compilation mode. (N.B. issue with linking)
# see vtune -h collect for all the options
# Profiling MPI applications: https://software.intel.com/en-us/vtune-cookbook-profiling-mpi-applications
#    https://software.intel.com/en-us/vtune-help-mpi-code-analysis
vtune-collect-raw: ensure_nersc_slurm verify_not_root warn_profile_opts
ifeq ($(VTUNE_COLLECT_OPTION),)
	$(error VTUNE_COLLECT_OPTION must be set. examples: hotspots, hpc, threading, memory-consumption, uarch-exploration, memory-access, etc.)
endif
ifneq ($(CRAYPE_LINK_TYPE), dynamic)
	$(warning CRAYPE_LINK_TYPE must be set to dynamic to run vtune. It is currently $(CRAYPE_LINK_TYPE).    export CRAYPE_LINK_TYPE=dynamic	)
endif
	@echo "VTune reminders: make sure to load vtune module, be using intel toolchain, and have already compiled the application; use dymamic linking (CRAYPE_LINK_TYPE=dynamic), then, use NX to view the output."
	@echo "WARNING: not recompiling binary -- you must have already compiled it using 'make'"
	@echo "COMMAND: "
	@echo "    $(RUN_DRIVER) vtune -collect=$(VTUNE_COLLECT_OPTION) -trace-mpi -result-dir vtune_out/$(VTUNE_COLLECT_OPTION).$(TIMESTAMP) $(APP_CMD)"

	$(RUN_DRIVER) vtune -collect=$(VTUNE_COLLECT_OPTION) -trace-mpi -result-dir vtune_out/$(VTUNE_COLLECT_OPTION).$(TIMESTAMP) $(APP_CMD)

#####
intel_tools_precheck: ensure_intel_tools ensure_nersc_slurm verify_not_root verify_profile_opts main-prebuild numactl_status

# VTune
# https://docs.nersc.gov/programming/performance-debugging-tools/vtune/
# deprecated until linking issue is fixed
#   Baseline VTune: consider using sbcast to broadcast out the executable to the nodes before use only for the baseline situation. See https://docs.nersc.gov/jobs/best-practices/#large-jobs
vtune-collect-base: intel_tools_precheck
	$(RUN_DRIVER) vtune -collect=$(VTUNE_COLLECT_OPTION) -trace-mpi -result-dir vtune_out/$(VTUNE_COLLECT_OPTION).$(TIMESTAMP) $(APP_CMD)
	@echo
	@echo "Now, in a temp directory on the laptop, run"
	@echo
	@echo "   vtune $(TEST_DIR)/$(shell ls -td -- vtune_out/* | head -n 1)"
	@echo 

vtune: VTUNE_COLLECT_OPTION ?= uarch-exploration
vtune: vtune-collect-raw

vtune-hotspots: 
	VTUNE_COLLECT_OPTION=hotspots $(MAKE) vtune-collect-raw

vtune-threading: 
	VTUNE_COLLECT_OPTION=threading $(MAKE) vtune-collect-raw

vtune-hpc-performance: 
	VTUNE_COLLECT_OPTION=hpc-performance $(MAKE) vtune-collect-raw

vtune-memory-consumption: 
	VTUNE_COLLECT_OPTION=memory-consumption $(MAKE) vtune-collect-raw

vtune-memory-access:
	VTUNE_COLLECT_OPTION=memory-access $(MAKE) vtune-collect-raw

vtune-gui: ensure_nersc_slurm
	@echo Run the following to open the gui with a particular file:
	@echo
	@echo "amplxe-gui <dir>"
	@echo

# open VTune GUI with latest collection
#vtune-gui-latest: MOST_RECENT_DIR = $(shell cd vtune_out; latest_dir)
vtune-gui-latest: MOST_RECENT_DIR = $(shell ls -dtA vtune_out/*/ | head -1)
vtune-gui-latest:
	echo Opening VTune GUI with most recent vtune: $(MOST_RECENT_DIR)
	amplxe-gui $(MOST_RECENT_DIR)*.amplxe

# https://docs.nersc.gov/programming/performance-debugging-tools/advisor/
advisor: intel_tools_precheck
	@echo "reminder to make (on an interactive node), run in profile mode, and module load advisor"
	mkdir -p advisor_out
	$(RUN_DRIVER) advixe-cl -collect survey -trace-mpi --project-dir advisor_out/$(TIMESTAMP) -- $(APP_CMD)
	@echo "Done running advisor. see advisor_out/$(TIMESTAMP) for reports."

###########################################################################
# Intel Trace Analyzer and APS

# https://www.nersc.gov/users/software/performance-and-debugging-tools/application-performance-snapshot-aps/
aps: RESDIR = aps_reports/$(TIMESTAMP)
aps: intel_tools_precheck
	@echo -e "$$KYEL""MAKE SURE TO MAKE CLEAN BEFORE RUNNING APS -- IT USES DIFFERENT COMPILERS (not doing it in the rule to allow consecutive runs)$$KRESET"
	$(RM_F) aps_report*.html
	$(RM_RF) aps_reports/*
	$(RUN_DRIVER) aps --result-dir=$(RESDIR) $(APP_CMD)
	aps --report=$(RESDIR)
	ruby -e "require ENV['CPL'] + '/support/experiments/html/report_helpers.rb' ; include ReportHelpers; publish_aps_file!"
	module swap PrgEnv-intel PrgEnv-gnu

# https://www.nersc.gov/users/software/performance-and-debugging-tools/intel-trace-analyzer-and-collector
trace: OUTDIR = itac/$(TIMESTAMP)
trace: intel_tools_precheck
ifeq ($(USE_TRACEANALYZER),)
	$(error USE_TRACEANALYZER in application Makefile must be set to a valid Trace Analyzer library (e.g., VT or VTim))
endif
	LD_PRELOAD=$(VT_ROOT)/slib/lib$(USE_TRACEANALYZER).so VT_PCTRACE=5 VT_LOGFILE_FORMAT=STFSINGLE $(RUN_CMD)	
	mkdir -p $(OUTDIR)
	mv *.stf* $(OUTDIR)
	mv *.prot $(OUTDIR)
	@echo "Trace Analyzer finished running with USE_TRACEANALYZER $(USE_TRACEANALYZER). View results with   traceanalyzer $(OUTDIR)/$(shell basename $(BIN_NAME)).single.stf"
	@echo

comm-graph: COMM_GRAPH_OUT := comm_graphs
comm-graph:
ifneq ($(TRACE_MPI_CALLS),1)
	$(error TRACE_MPI_CALLS must be set to 1 for this target.)
endif
	@echo REMINDER: you must 'make run' the program first before running this
	mkdir -p $(COMM_GRAPH_OUT)
	rm -f $(COMM_GRAPH_OUT)/*.png
	Rscript $(CPL)/support/experiments/graph_utils/plot_comm_graph.R logs $(COMM_GRAPH_OUT) $(PURE_RT_NUM_THREADS) $(NPROCS)
ifeq ($(OS), osx)
	open $(COMM_GRAPH_OUT)/*.png
endif
	@echo "TIP: It's recommended to also analyze communication using:   excel logs/__annotator_outfile.csv"



###########################################################################
# https://docs.nersc.gov/programming/performance-debugging-tools/craypat/#how-to-use-craypat
craypat-howto:
	@echo "INSTRUCTIONS TO DO A CRAYPAT PROFILE:"
	@echo "  You will most likely want to run this on the baseline MPI program or a pure single thread overridden runtime."
	@echo 
	@echo "LOGIN WINDOW:        make clean && module load perftools-base perftools && make craypat-build"
	@echo "INTERACTIVE WINDOW:  module load perftools-base perftools  && make craypat"
	@echo "                     pat_report <output file>"
	@echo 
	@echo "...then review the results. You may want to copy the MPICH_RANK_REORDER up to the application directory."
	@echo "   to actually use the rank-reorder file, run like this: MPICH_RANK_REORDER_METHOD=3 MPICH_RANK_REORDER_DISPLAY=1 mrr"
	@echo "You'll probably also want to use the gen_threadmaps.rb script to generate MPI and Pure threadmap files. Run that script with no arguments to recall how to properly generate threadmap files."
	@echo

PAT_BIN=./$(BIN_NAME_NO_PATH)+pat
craypat-build:
	@echo "EXPERIMENTAL -- make clean before running this!"
	@echo
	$(warning **** Before main-prebuild: $(__OBJ_DIR_PARAMS))
	$(MAKE) main-prebuild
	$(RM_F) $(PAT_BIN)
	$(warning **** Before Pat build: $(__OBJ_DIR_PARAMS))
	pat_build -g mpi $(BIN_NAME)
	@echo
	@echo Done modifying the binary $(BIN_NAME) with pat_build
	@echo
	
craypat: 
	JSON_STATS_DIR=$(JSON_STATS_DIR) $(RUN_DRIVER) $(PAT_BIN) $(RUN_ARGS) $(INTERACTIVE_SUFFIX)
	@echo
	@echo run something like   pat_report $(PAT_BIN)+
	@echo
	@echo "TIP: you can find the MPI Rank order file in the above dir (MPICH_RANK_ORDER.Grid). THen run wtih MPICH_RANK_REORDER_METHOD=3 mrr"
	@echo "TIP: clean up craypat binaries and reports with 'make craypat-clean'"

craypat-clean:
	$(RM_F) $(BIN_PREFIX)*+pat
	$(RM_RF) $(BIN_PREFIX)*+pat+*



# sample run: # sudo /afs/csail.mit.edu/u/j/jim/local/src/pcm-master/pcm.x 0.10 -- bash -c 'echo "7^199999" | bc > /dev/null'
# note: PCM_BIN can be overridden with other PCM tools, such as pcm-numa.x or pcm-memory.x
pcm: SAMPLE_INTERVAL = 0.10
pcm: PCM_BIN = $(shell which pcm.x)
pcm: CSV_OUT = pcm_log.csv
pcm: verify_root verify_profile_opts main-prebuild numactl_status 
	@echo "████████████████████████████████████████████████████████████████████████████████████"
	$(PCM_BIN) $(SAMPLE_INTERVAL) -csv=$(CSV_OUT) -- $(RUN_CMD)
	@echo "████ SUCCESS (exit code 0) █████████████████████████████████████████████████████████"
	@echo See $(CSV_OUT) for pcm report.

toplev: TOPLEV_CSV = toplev.csv
toplev: TOPLEV_GRAPH_FORMAT = pdf
toplev: TOPLEV_LEVEL = 2
toplev: verify_not_root verify_profile_opts main-prebuild numactl_status
	@echo "WARNING: you will likely have to fix up some performance counter permissions issues using sudo, unless you already did it on this machine."
	@echo "████████████████████████████████████████████████████████████████████████████████████"
	# NUMACTL_VERBOSE_CORES is not properly generalized for all architectures. Either do that or get around this argument.
	#toplev.py -l$(TOPLEV_LEVEL) --csv , --output $(TOPLEV_CSV) -I 100 $(RUN_CMD)
	$(SUPPORT_PATH)/misc/gen_toplev_graphs.rb --toplev-csv=$(TOPLEV_CSV) --numactl-verbose-cores=$(NUMACTL_VERBOSE_CORES) --output-format=$(TOPLEV_GRAPH_FORMAT)
	@echo "████ SUCCESS (exit code 0) █████████████████████████████████████████████████████████"

ucevent: UCEVENTS = CBO.AVG_LLC_DATA_READ_MISS_LATENCY CBO.MEM_WB_BYTES CBO.LLC_DDIO_MEM_READ_BYTES CBO.LLC_DDIO_MEM_WRITE_BYTES CBO.LLC_PCIE_MEM_READ_BYTES CBO.LLC_PCIE_MEM_WRITE_BYTES iMC.MEM_BW_READS iMC.MEM_BW_WRITES QPI_LL.QPI_SPEED QPI_LL.DATA_FROM_QPI QPI_LL.DATA_FROM_QPI_TO_NODE0 QPI_LL.DATA_FROM_QPI_TO_NODE1 QPI_LL.QPI_LINK_UTIL 
ucevent: UCEVENT_CSV = ucevent.csv
ucevent: UCEVENTS_MS_INTERVAL = 100
ucevent: verify_not_root verify_profile_opts main-prebuild numactl_status
	@echo "████████████████████████████████████████████████████████████████████████████████████"
	ucevent.py -o $(UCEVENT_CSV) -I 100
	ucevent.py -o ucevent.csv -I $(UCEVENTS_MS_INTERVAL) --csv , $(UCEVENTS)
	@echo "████ SUCCESS (exit code 0) █████████████████████████████████████████████████████████"
	@echo "See $(UCEVENT_CSV) for CSV output file from ucevent. Modify UCEVENTS environment variable to add or remove events."


thread-timeline-verify:
ifneq ($(COLLECT_THREAD_TIMELINE_DETAIL),1)
	$(error "COLLECT_THREAD_TIMELINE_DETAIL must be set to 1 to run this target")
endif

thread-timeline-run: thread-timeline-verify
	$(MAKE) run 
	$(MAKE) thread-timeline

# doesn't currently work on NERSC due to CSS and JS files not being in the right place
# probably need to copy them to web dir and point to the right place
thread-timeline: JSON_STATS_DIR ?= runs/temp_latest
thread-timeline: TT_TIMEILNE_WIDTH ?= 1450
thread-timeline: thread-timeline-verify
	@echo Generating thread timeline. Set width with TT_TIMEILNE_WIDTH. Set env vars TT_BEGIN and TT_END to truncate timeline visual
	@echo JSON_STATS_DIR is $(JSON_STATS_DIR)
	$(eval HTTPDIR := $(shell ruby -e "require '$(CPL)/support/experiments/html/report_helpers' ; include ReportHelpers; puts http_doc_directory('thread_timelines/$(BIN_NAME_NO_PATH)')"))
	mkdir -p $(HTTPDIR)
	$(eval ASSET_BASE_DIR := $(shell ruby -e "require '$(CPL)/support/experiments/html/report_helpers' ; include ReportHelpers; puts http_doc_directory()"))
ifeq ($(OS),$(filter $(OS),cag nersc))
	cp -rf $(CPL)/support/experiments/html/css $(ASSET_BASE_DIR)
	cp -rf $(CPL)/support/experiments/html/javascript $(ASSET_BASE_DIR)
endif
	$(CPL)/support/experiments/html/thread_timeline.rb $(BIN_NAME) . $(PROCESS_CHANNEL_VERSION) '$(PROCESS_CHANNEL_BUFFERED_MSG_SIZE)' $(JSON_STATS_DIR) $(TT_TIMEILNE_WIDTH) '$(RUN_ARGS)' $(TT_BEGIN) $(TT_END)
	mv thread_timeline.html $(HTTPDIR)
	ruby -e "require '$(CPL)/support/experiments/html/report_helpers' ; include ReportHelpers; publicize_files!('$(HTTPDIR)')"
	$(eval WEB_URL := $(shell ruby -e "require '$(CPL)/support/experiments/html/report_helpers' ; include ReportHelpers; puts web_url('thread_timelines/$(BIN_NAME_NO_PATH)', nil, 'thread_timeline.html')"))
	@echo
	@echo TIP: Set width with TT_TIMEILNE_WIDTH. Set env vars TT_BEGIN and TT_END to truncate timeline visual
	@echo
	@echo $(WEB_URL)
	@echo
ifeq ($(OS), osx)
	open $(WEB_URL)
endif

#shortcuts
tt: thread-timeline
ttr: thread-timeline-run

ranks-on-topo:
	$(SUPPORT_PATH)/misc/label_ranks_on_core_topo.rb $(JSON_STATS_DIR)/rank_to_cpu_map.csv
	@echo "See output PDF files in the topology directory. Open files over X using   evince topology/*.png"
	@echo
	
bloaty: ensure_cag_machine
	OBJ_DIR_PREFIX=$(OBJ_DIR_PREFIX) LINK_TYPE=$(LINK_TYPE) $(MAKE) -C $(LIBPURE_MAKEFILE_PATH) bloaty

cat_build_opts:
	@echo 
	@echo BUILD OPTIONS:
	[[ -e $(BUILD_PREFIX)/build_options.txt ]] && cat $(BUILD_PREFIX)/build_options.txt || echo ''
	@echo 
