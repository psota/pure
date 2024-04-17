# Author: James Psota
# File:   Makefile.run_config.mk 

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

# this file sets up the run command for all of the different system configurations

# Error checking
ifndef PURE_RT_NUM_THREADS
$(error PURE_RT_NUM_THREADS is not set, but must be set to either and integer or AUTO. If you are not multithreading, just set PURE_RT_NUM_THREADS to 1.)
endif

# build up run command
APP_CMD = $(BIN_NAME) $(RUN_ARGS)
BATCH_JOB_NAME = $(shell echo $(BIN_NAME_NO_PATH) $(RUN_ARGS) $(RUNTIME) | tr ' ' '_')

# MPI and Pure modes
# run using SLURM
# TODO: handle thread binding. Can we use NUMACTL or something else on the NERSC machines?
# TODO: other options to srun?
# TODO: handle both interactive mode and batch submission mode, ideally automatically
# TODO: review https://my.nersc.gov/script_generator.php for all parameters

# special override option for runtime for batch mode to pass in something other than Pure or MPI (defined in makefiles) to node_config
ifneq ($(OVERRIDE_RUNTIME),)
$(warning OVERRIDING RUNTIME FROM $(RUNTIME) to $(OVERRIDE_RUNTIME))   
RUNTIME = $(OVERRIDE_RUNTIME)
endif

# see https://slurm.schedmd.com/srun.html for all srun options
# TODO: optionally scale up to larger timeframes than 30 min

# for some reason the ?= syntax isn't workign for THREADS_PER_NODE_LIMIT
# ifndef THREADS_PER_NODE_LIMIT
#   THREADS_PER_NODE_LIMIT = nil
# endif

$(warning IN RUN MAKEFILE: ENABLE_HYPERTHREADS = $(ENABLE_HYPERTHREADS))


ENABLE_HYPERTHREADS ?= true

AUTO_FLAG := AUTO

ifdef THREADS_PER_NODE_LIMIT
  RUBY_THREADS_PER_NODE_LIMIT = $(THREADS_PER_NODE_LIMIT)
else
  RUBY_THREADS_PER_NODE_LIMIT = nil
endif

ifeq ($(NPROCS), $(AUTO_FLAG))

  ifeq (,$(findstring Pure,$(RUNTIME)))
    $(error NPROCS set to $(AUTO_FLAG) but AUTO mode is only supported for the Pure runtime (RUNTIME is $(RUNTIME)).)
  endif

  ifneq ($(PURE_RT_NUM_THREADS), $(AUTO_FLAG))
    $(error NPROCS is set to $(NPROCS) so PURE_RT_NUM_THREADS must also be set to $(AUTO_FLAG))
  endif

  export NPROCS = $(shell ruby --disable=gems -e "require ENV['CPL'] + '/support/experiments/benchmarks/machine_helpers.rb' ; puts MachineHelpers.node_config('$(RUNTIME)', $(TOTAL_THREADS), $(ENABLE_HYPERTHREADS), $(RUBY_THREADS_PER_NODE_LIMIT), nil, :nprocs_only)")
  export PURE_NUM_PROCS = $(NPROCS) 
  export PURE_RT_NUM_THREADS = $(shell ruby --disable=gems -e "require ENV['CPL'] + '/support/experiments/benchmarks/machine_helpers.rb' ; puts MachineHelpers.node_config('$(RUNTIME)', $(TOTAL_THREADS), $(ENABLE_HYPERTHREADS), $(RUBY_THREADS_PER_NODE_LIMIT), nil, :pure_threads_only)")
  $(warning Running in AUTO mode with TOTAL_THREADS=$(TOTAL_THREADS). Setting PURE_NUM_PROCS and NPROCS automatically to $(NPROCS); PURE_RT_NUM_THREADS automatically to $(PURE_RT_NUM_THREADS))
endif

ifeq ($(OS),nersc)

    #SLURM_PARTITION ?= debug
    # don't specify a job length for sbatch jobs because the sbatch job already has its own time limit
  
    # can I get away with not using this?
     #ifneq ($(SLURM_NO_SRUN_JOB_LIMIT),1)
   #   SLURM_JOB_LEN ?= -t 00:14:58
   # endif
    ifeq ($(NPROCS), $(AUTO_FLAG))
      # set this for the upcoming Ruby call to machine_helpers.rb
      NPROCS = nil
    endif

    ifeq ($(NPROCS), $(AUTO_FLAG))

      $(warning nprocs is auto flag. runtime is $(RUNTIME))

      SLURM_NODE_OPTIONS = $(shell ruby --disable=gems -e "require ENV['CPL'] + '/support/experiments/benchmarks/machine_helpers.rb' ; puts MachineHelpers.node_config('$(RUNTIME)', $(TOTAL_THREADS), $(ENABLE_HYPERTHREADS), $(RUBY_THREADS_PER_NODE_LIMIT), nil, :slurm, $(OMP_NUM_THREADS))")
    else
      ifeq ($(NPROCS), RAW_SLURM_OPTS)
$(warning Using RAW_SLURM_OPTS mode.)
        SLURM_NODE_OPTIONS = $(RAW_SLURM_OPTS)
      else
              $(warning nprocs is NOT auto flag. runtime is $(RUNTIME))
        SLURM_NODE_OPTIONS = $(shell ruby --disable=gems -e "require ENV['CPL'] + '/support/experiments/benchmarks/machine_helpers.rb' ; puts MachineHelpers.node_config('$(RUNTIME)', $(TOTAL_THREADS), $(ENABLE_HYPERTHREADS), $(RUBY_THREADS_PER_NODE_LIMIT), nil, :slurm, $(OMP_NUM_THREADS))")
      endif
    endif

    ifeq ($(shell ruby --disable=gems -e "require ENV['CPL'] + '/support/experiments/benchmarks/machine_helpers.rb' ; puts MachineHelpers.node_config('$(RUNTIME)', $(TOTAL_THREADS), $(ENABLE_HYPERTHREADS), $(RUBY_THREADS_PER_NODE_LIMIT), nil, :nprocs_only)"), 1)
      MPI_ENABLED = 0
    else
      MPI_ENABLED = 1
    endif

    # TODO: verify that NUMACTL srun works
    # hack -- taking out "cores" binding

#    RUN_DRIVER = $(NUMACTL) srun $(SLURM_NODE_OPTIONS) --licenses=cscratch1 --cpu_bind=verbose,cores --job-name $(BATCH_JOB_NAME) 
    # note: using map_cpu approach!  
    ifeq ($(NUMACTL_CPUS),)
      # if no NUMACTL CPUS specified
      ifeq ($(OMP_NUM_THREADS), 1)
        CPU_BIND = --cpu_bind=verbose,cores
      else
        # open mp -- don't bind and let OMP do it
        CPU_BIND =
      endif
    else
      # This is really verbose but the Make syntax is ugly.
      ifeq ($(RUNTIME), MPI)
        CPU_BIND = --cpu_bind=verbose,map_cpu:$(NUMACTL_CPUS)
      endif

      ifeq ($(RUNTIME), PureSingleThread)
        CPU_BIND = --cpu_bind=verbose,map_cpu:$(NUMACTL_CPUS)
      endif

      ifeq ($(PURE_AS_MPI_MODE), 1)
        CPU_BIND = --cpu_bind=verbose,map_cpu:$(NUMACTL_CPUS)
      endif

      ifeq ($(RUNTIME), PureSingleThreadPerNode)
        CPU_BIND = --cpu_bind=verbose,map_cpu:$(NUMACTL_CPUS)
      endif

      ifeq ($(RUNTIME), Pure)
        CPU_BIND = --cpu_bind=verbose,cores
      endif

      ifeq ($(RUNTIME), PureProcPerNUMA)
        CPU_BIND = --cpu_bind=verbose,cores
      endif
      # TODO: add other runtimes as needed
    endif

    RUN_DRIVER = srun $(SLURM_NODE_OPTIONS) $(CPU_BIND) --job-name $(BATCH_JOB_NAME) 

    # append on output file regardless of MPI running or not.
    ifneq ($(SLURM_OUTPUT_FILE),)
      # direct to output file if SLURM_OUTPUT_FILE is set
      RUN_DRIVER += --output $(SLURM_OUTPUT_FILE).%j --error $(SLURM_OUTPUT_FILE).error.%j
    endif

else
  # run directly / interactively using mpirun
  # OSX / CAG in MPI-mode
  #$(warning Not running on NERSC machines (should be OSX or cag). Configuring to run directly (not using SLURM).)
  ifeq ($(NPROCS), 1)
    # run directly -- no MPI
    RUN_DRIVER = $(NUMACTL)
    MPI_ENABLED = 0
  else
    RUN_DRIVER = $(NUMACTL) $(MPIRUN) -np $(NPROCS) $(MPIRUN_OPTS)
    MPI_ENABLED = 1
  endif

endif


# ampi mode -- total override! Just don't use NERSC_COMPILER_OVERRIDE override on OSX
ifeq ($(RUNTIME_IS_AMPI),1)
  # assume it's a reasonable value
  # hack mode -- use a hacked version.
  RUN_DRIVER = charmrun_pure +p$(AMPI_PHYS_PROCS) 
  APP_CMD  += $(AMPI_OPTS)
endif


INTERACTIVE_SUFFIX = 2>&1 | tee $(RUN_OUTPUT_FILENAME) ; test $${PIPESTATUS[0]} -eq 0 ;

# add in jemalloc
ifeq ($(USE_JEMALLOC),1)
  ifneq ($(MALLOC_CONF),)
    RUN_DRIVER := MALLOC_CONF=$(MALLOC_CONF) $(RUN_DRIVER) 
  endif
endif

# hopefully I'll eventually figure this out. Right now, JSON_STATS_DIR is somehow getting lost in the environment so passing it along again.
export MPI_ENABLED
RUN_CMD = OMP_NUM_THREADS=$(OMP_NUM_THREADS) $(RUN_DRIVER) $(APP_CMD)

$(warning RUN_CMD is *******************************************************)
$(warning $(RUN_CMD))
