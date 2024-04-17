# Author: James Psota
# File:   machine_helpers.rb 

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

# frozen_string_literal: true

module MachineHelpers
  def self.running_on_cag?
    ENV['OS'] == 'cag'
  end

  def self.running_on_osx?
    ENV['OS'] == 'osx'
  end

  def self.running_on_nersc?
    ENV['OS'] == 'nersc'
  end

  def self.running_on_perlmutter?
    ENV['NERSC_HOST'] == 'perlmutter'
  end

  def self.perlmutter_compute_node?
    matcher = `hostname`.strip.match(/^nid\d{6}/)
    return !matcher.nil?
  end

  def self.git_branch 
    return `git rev-parse --abbrev-ref HEAD`.strip
  end

  def self.num_cores(include_smt_hw_threads = true)
    # Note: I wanted to just use something like $(shell nproc) but it gets complicated with NERSC login nodes, etc. so hardcoding
    case ENV['OS']
    when 'osx'
      include_smt_hw_threads ? 8 : 4
    when 'cag'
      case ENV['CPU_MODEL']
      when 'Intel(R) Xeon(R) CPU E5-2680 v3 @ 2.50GHz'
        # salike and lanka-v3
        include_smt_hw_threads ? 48 : 24
      when 'Intel(R) Xeon(R) CPU E5-2695 v2 @ 2.40GHz'
        # lanka research (v2)
        include_smt_hw_threads ? 48 : 24
      when 'Intel(R) Xeon(R) CPU E5-2670 0 @ 2.60GHz'
        # draco
        include_smt_hw_threads ? 32 : 16
      else
        raise "No config for CPU_MODEL #{ENV['CPU_MODEL']}"
      end

    when 'nersc'
      case ENV['NERSC_HOST']
      when 'cori'
        case ENV['CRAY_CPU_TARGET']
        when 'haswell'
          include_smt_hw_threads ? 64 : 32
        when 'mic-knl'
          # As recommended by NERSC, only running with 64 cores
          # include_smt_hw_threads ? 272 : 68
          include_smt_hw_threads ? 256 : 64
        else
          raise 'Invalid CRAY_CPU_TARGET on NERSC Cori machine. Must be either haswell or mic-knl to determine num_cores'
        end
      when 'perlmutter'
        case ENV['CRAY_CPU_TARGET']
        when 'x86-milan'
          include_smt_hw_threads ? 256 : 128
        else
          raise 'Invalid CRAY_CPU_TARGET on NERSC Perlmutter machine. Must be x86-milan to determine num_cores'
        end
      else
        raise "Invalid NERSC machine name: #{ENV['NERSC_HOST']}"
      end
    else
      raise "num_cores not implemented for 'os' env variable #{os}"
    end
      end

  # returns options that are acceptable to srun
  # total_threads is the number of threads that run main -- doesn't include omp worker threads
  def self.node_config(runtime, total_threads, use_hyperthreads, threads_per_node_max = nil, nprocs_set_by_hand_validation = nil, mode = :slurm, omp_num_threads = nil)

    if(use_hyperthreads == 0)
      use_hyperthreads = false
    end

    $stderr.puts "node_config: use_hyperthreads: #{use_hyperthreads}"

    #$stderr.puts "input omp_num_threads = #{omp_num_threads}"

    runtime = runtime.strip

    if(omp_num_threads&.> 1)
      raise "If using OMP threads, runtime must be MPI. Callstack above." unless runtime.match /^MPI/
      # hack runtime to be "OMP" to be used below
      runtime = 'OMP'
    end

    # nprocs_set_by_hand_validation is only for verification purposes. It's NOT an override. It's intended to make sure other makefiles and
    # mechanisms to set NPROCS is correct.
    cores_avail_per_node = num_cores(use_hyperthreads)

    # first, just use cores avail per node or the overriding threads_per_node_max
    # we may be fixing this up later if there's an imbalance of threads per node
    if threads_per_node_max.nil?
      using_cores_per_node = cores_avail_per_node
    else
      # this is for a situation where we want to artificially decrease the number of threads per node, perhaps if each thread uses a lot of memory
      using_cores_per_node = [cores_avail_per_node, threads_per_node_max].min
    end

    if threads_per_node_max && (threads_per_node_max > cores_avail_per_node)
      # puts "Not allowed to pass in a higher threads_per_node_max than the number of cores per node on this machine (#{cores_avail_per_node})".red
      raise "Not allowed to pass in a higher threads_per_node_max (#{threads_per_node_max}) than the number of cores per node on this machine (#{cores_avail_per_node})".red
    end

    omp_total_threads = total_threads * (omp_num_threads || 1)

    #$stderr.puts "omp_total_threads is #{omp_total_threads}"
    raise "omp_total_threads is NaN" if omp_total_threads.to_f.nan?

    # now, figure out how many nodes we need
    # pure strategy: single process per node, packing as many threads as possible into that process
    slurm_nprocs = ENV['SLURM_JOB_NUM_NODES']&.to_i
    if running_on_nersc? || !slurm_nprocs.nil?

     # $stderr.puts "nodes calc -- omp_total_threads #{omp_total_threads} and using_cores_per_node #{using_cores_per_node}"

      nodes = (omp_total_threads.to_f  / using_cores_per_node.to_f).ceil
      unless running_on_nersc?
        unless nodes <= slurm_nprocs
          raise "Calculated number of nodes (#{nodes}) should be equal to the number of allocated SLURM nodes (#{slurm_nprocs})"
           end
      end
    else
      nodes = 1
    end

    # finally, rebalance out the threads if necessary, keeping the number of nodes fixed
    # only do this if threads_per_node_max wasn't passed in
    if threads_per_node_max.nil?
      #$stderr.puts "nodes is #{nodes}"

      using_cores_per_node = (omp_total_threads&.to_f / nodes&.to_f).ceil
    end

    ### now configure max_tasks_per_node and cpus_per task depending on runtime
    if runtime == 'MPI' || runtime == 'MPI_DMAPP' || runtime == 'PureSingleThread' || runtime == 'PureSingleThreadPerNode'
      cpus_per_task = 1
      max_tasks_per_node = if nodes == 1
                             total_threads
                           else
                             using_cores_per_node
                           end

      total_tasks = total_threads
    elsif runtime == 'Pure'
      # terrible hack but try this.
      max_tasks_per_node = 1
      cpus_per_task = if nodes == 1
                        total_threads
                      else
                        using_cores_per_node
                      end
      total_tasks = nodes # one task per node
    elsif runtime == 'OMP'
      # there can be any number of omp threads per tasks, but make sure it's sane
      raise "omp_num_threads must be set and non-zero to make a node configuration" unless !omp_num_threads.nil? && omp_num_threads > 0
      cpus_per_task = omp_num_threads
      raise "omp_num_tasks (#{omp_num_tasks} must evenly divide the number of cores used per node (#{using_cores_per_node}" unless using_cores_per_node % omp_num_threads == 0

      max_tasks_per_node = if nodes == 1
                            total_threads
                          else
                            using_cores_per_node / omp_num_threads
                          end
      total_tasks = max_tasks_per_node * nodes 
    elsif runtime == 'PureProcPerNUMA'
      # I think the new implementatoin of PureProcPerNUMA in PureProcess.cpp allows an odd number of threads per node. Testing this.
      # if using_cores_per_node.odd?
      #   raise "currently not supporting a non even using_cores_per_node (tried to use #{using_cores_per_node})"
      #  end
      unless ENV['CRAY_CPU_TARGET'] == 'haswell'
        raise "runtime PureProcPerNUMA only works on Cori haswell for now. KNL only has one NUMA node, so just don't run a KNL job with PureProcPerNUMA."
      end

      max_tasks_per_node = 2 # hardcoded to haswell (KNL shouldn't use numa proc nodes)
      cores_per_numa_node = using_cores_per_node / 2

      if nodes == 1
        # WARNING -- this branch is not vetted. Check numbers.
        # if there's just one node needed, we use one NUMA node if possible, and two otherwise
        if total_threads <= cores_per_numa_node
          total_tasks = 1
          cpus_per_task = total_threads
        else
          total_tasks = 2
          cpus_per_task = (using_cores_per_node / total_tasks.to_f).ceil
        end
      else
        cpus_per_task = cores_per_numa_node
      end
      total_tasks = 2 * nodes # two tasks (MPI processes) per node
    elsif runtime == 'PureProcPerNUMAMilan'
      unless ENV['NERSC_HOST'] == 'perlmutter'
        raise "runtime PureProcPerNUMAMilan only works on Perlmutter nodes for now."
      end

      max_tasks_per_node = 8
      cores_per_numa_node = cores_avail_per_node / max_tasks_per_node

      total_tasks   = (total_threads.to_f / cores_per_numa_node.to_f).ceil
      cpus_per_task = cores_per_numa_node # does this work?
    else
      raise "Unsupported runtime #{runtime}"
    end

    if (ENV['PRINT_NODE_CONFIG']&.to_i) == 1
      $stderr.puts
      warn "mode: #{mode}"
      warn "runtime: #{runtime}"
      warn "slurm_nprocs: #{slurm_nprocs}"
      warn "cores_avail_per_node: #{cores_avail_per_node}"
      warn "nodes #{nodes}"
      warn "using_cores_per_node #{using_cores_per_node}"
      warn "max_tasks_per_node: #{max_tasks_per_node}"
      warn "cpus_per_task: #{cpus_per_task}"
      warn "threads_per_node_max: #{threads_per_node_max}"
      warn "omp_num_threads: #{omp_num_threads}"
      warn "total_threads: #{total_threads}"
    end

    if !nprocs_set_by_hand_validation.nil? && total_tasks != nprocs_set_by_hand_validation
      node_check_msg = "nprocs_set_by_hand_validation arg of #{nprocs_set_by_hand_validation} not consistent with what was calculated by #{__method__}, which was #{total_tasks} (assuming we want to run #{using_cores_per_node} threads per node on this machine)."
      if osx_host?
        # warn this but because of complications from PureSingleThread, don't fail. May want to fix this.
        warn node_check_msg.yellow
      else
        raise node_check_msg.red
      end
    end

    ### now format output as desired
    case mode
    when :slurm
      if runtime == 'MPI' || runtime == 'MPI_DMAPP' || runtime == 'PureSingleThread'
        # Note: sometimes ntasks-per-node will be ignored (when nodes > 1 and the machine is not fully packed).
        # For code simplicity, we let SLURM warn about that and handle this for us.
        #        "--ntasks=#{total_threads} --nodes=#{nodes} --ntasks-per-node=#{max_tasks_per_node} --cpus-per-task=#{cpus_per_task}"

        # 2023: removing cpus-per-task argument -- sure why this is a problem to have just one listed.
        # old logic -- just give all of the cores to each task and let them sit idle
        # see email from Helen (NERSC): https://mail.google.com/mail/u/0/#inbox/FMfcgzGrcrmcGCMVjVRWPJBmLwpHNhhM
        # apparently something changed and we need to use more cores per task on a single node

        "--ntasks=#{total_threads} --nodes=#{nodes} --ntasks-per-node=#{max_tasks_per_node}"
      elsif runtime == 'PureSingleThreadPerNode'
        # for some special intranode performance testing
        "--ntasks=#{total_threads} --nodes=#{total_threads} --ntasks-per-node=1 --cpus-per-task=1"
      elsif runtime == 'Pure'
        # For pure, we don't pass in ntasks to slurm and let the Pure runtime sort out number of threads per process
        if ENV['PURE_RT_NUM_THREADS'].to_i == 1 && runtime != 'OMP'
          raise 'If you want to run one thread per MPI process, use the special PureSingleThread runtime mode'
        end


        # hack to add full flexibilty for binding ranks on any core on perlmutter
        if(use_hyperthreads == false)
          cores_avail_per_node *= 2
        end
        "--nodes=#{nodes} --ntasks-per-node=#{max_tasks_per_node} --cpus-per-task=#{[cpus_per_task, cores_avail_per_node].max}"
      elsif runtime == 'OMP'
        "--nodes=#{nodes} --ntasks-per-node=#{max_tasks_per_node} --cpus-per-task=#{cpus_per_task}"
      elsif runtime == 'PureProcPerNUMA'
        # For pure, we don't pass in ntasks to slurm and let the Pure runtime sort out number of threads per process
        if ENV['PURE_RT_NUM_THREADS'].to_i == 1 && runtime != 'OMP'
          raise 'If you want to run one thread per MPI process, use the special PureSingleThread runtime mode'
        end
        # use terrible hack from nersc
        "--nodes=#{nodes} --ntasks-per-node=#{max_tasks_per_node} --cpus-per-task=#{[cpus_per_task, cores_avail_per_node/2].max}"
      elsif runtime == 'PureProcPerNUMAMilan'
        "--nodes=#{nodes} --ntasks-per-node=#{total_tasks} --cpus-per-task=#{cpus_per_task}"
      else
        raise 'Invalid runtime: ' + runtime
       end
    when :array
      [nodes, max_tasks_per_node, cpus_per_task]
    when :env_vars
      # here, total_tasks just means the total tasks for the program across all nodes
      # this mode is only used for validation later in the real srun call
      # or to max out the number of threads based on the machine you are running on
      "NPROCS=#{total_tasks} PURE_NUM_PROCS=#{total_tasks} PURE_RT_NUM_THREADS=#{cpus_per_task} THREADS_PER_NODE_LIMIT=#{threads_per_node_max || 'nil'}"
    when :nodes_only
      nodes
    when :nprocs_only
      total_tasks
    when :pure_threads_only
      cpus_per_task
    when :nprocs_and_pure_threads
      [total_tasks, cpus_per_task]
    else
      raise "invalid mode #{mode}".red
    end
  end
end
