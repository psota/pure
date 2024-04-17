# Author: James Psota
# File:   benchmark_helpers.rb 

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

require 'fileutils'
require 'colorize'
require 'pty'
require 'descriptive_statistics'
require 'awesome_print'
require 'open3'
require 'shellwords'

require_relative "#{ENV['CPL']}/support/experiments/html/report_helpers"
include ReportHelpers

require_relative "#{ENV['CPL']}/support/experiments/benchmarks/machine_helpers"
include MachineHelpers

module BenchmarkHelpers
  ## Invariant: a runset is a set of runs that all leverage the same pure library configuration,
  ## and therefore the pure library doesn't need to be recompiled within the RunSet. However, it
  ## is recompiled once before the RunSet is run.
  class RunSet
    NA_PAYLOAD_COUNT = 'NO_PAYLOAD_COUNT_SPECIFIED'
    NA_NUMA_SCHEME = 'none'
    NA_TOPOLOGY = 'NO_TOPOLOGY_SPECIFIED'
    NA_CLASS = 'NO_CLASS_SPECIFIED'
    NA_PCV = -1
    NA_PCV_CHUNKS = -2
    NA_PC_BUF_SIZE = -3
    NA_OMP_NUM_THREADS = 0

    VALID_OVERRIDE_RUNTIMES = %w[PureSingleThread PureSingleThreadPerNode PureProcPerNUMA].freeze

    attr_reader :test_subdir, :runs_per_config, :message_count, :total_threads,
                :threads_per_node_max,
                :thread_map_filename, :payload_counts,
                :numa_schemes, :topology, :extra_description, :process_channel_versions, :process_channel_chunks,
                :pc_buf_msg_sizes, :klass, :run_args_custom, :extra_set_env_vars,
                :override_runtime, :jemalloc_options, :helper_threads_options, :use_asmlib,
                :pre_run_cmd, :post_run_cmd, :omp_num_threads, :max_buffered_chan_bytes_opts

    # only paramters that do not necessitate a recompilation should be array fields.
    ARRAY_FIELDS = %i[payload_counts numa_schemes process_channel_versions process_channel_chunks pc_buf_msg_sizes jemalloc_options helper_threads_options max_buffered_chan_bytes_opts].freeze
    BOOLEAN_FIELDS = %i[jemalloc_options helper_threads_options].freeze

    def initialize(test_subdir:, runs_per_config:, message_count:, total_threads: nil, threads_per_node_max: nil,
                   payload_counts: [NA_PAYLOAD_COUNT], thread_map_filename: nil,
                   numa_schemes: [NA_NUMA_SCHEME], topology: NA_TOPOLOGY, extra_description: nil, process_channel_versions: [NA_PCV], process_channel_chunks: [NA_PCV_CHUNKS],
                   pc_buf_msg_sizes: [NA_PC_BUF_SIZE], klass: NA_CLASS, run_args_custom: nil, extra_set_env_vars: nil, override_runtime: nil, jemalloc_options: [false],
                   helper_threads_options: [false],
                   use_asmlib: false, pre_run_cmd: nil, post_run_cmd: nil, omp_num_threads: nil, max_buffered_chan_bytes_opts: nil)

      method(__method__).parameters.each do |type, k|
        next unless type.to_s.match(/^key/)

        v = eval(k.to_s)
        if v
          validate_value(k, v)
          instance_variable_set("@#{k}", v)
        end
      end

      # create a richer extra description to fully encapsulate characteristics of this run
      create_set_extra_description!
    end

    private def create_set_extra_description!
      is_pure = runtime == 'Pure' || runtime == 'PureSingleThread' || runtime == 'PureSingleThreadPerNode' || runtime == 'PureProcPerNUMA'
      parts = []
      parts << "#{threads_per_node_max}t/n" if threads_per_node_max && is_pure
      parts << 'tmap' if !thread_map_filename.nil? && is_pure
      parts << 'asm' if use_asmlib && is_pure
      parts << "omp-#{omp_num_threads}" if omp_num_threads
      parts << extra_description if extra_description # in case passed in by user

      @extra_description = parts.join('-').squeeze('-')
    end

    private def create_config_extra_description(numa_scheme, pcv_chunks, use_jemalloc, use_helpers, max_buffered_chan_bytes)
      new_parts = []
      delim = '-'
      new_parts << numa_scheme + delim if numa_scheme != NA_NUMA_SCHEME

      if pcv_chunks > 999_999
        raise "format for chunks doesn't allow more than 999999 chunks; fix below."
        end

      if pcv_chunks != NA_PCV_CHUNKS
        new_parts << gen_val_description(pcv_chunks, 'chk', 'd', 6)
        end

      new_parts << 'je'   if use_jemalloc
      new_parts << 'HELP' if use_helpers
      new_parts << "bufB=#{max_buffered_chan_bytes}" if max_buffered_chan_bytes

      (@extra_description + delim + new_parts.join(delim)).squeeze(delim).chomp(delim)
    end

    def runtime
      if override_runtime
        unless VALID_OVERRIDE_RUNTIMES.include?(override_runtime)
          raise "Invalid override runtime '#{override_runtime}'"
          end

        override_runtime
      else
        if test_subdir.match /pure/
          'Pure'
        elsif test_subdir.match(/baseline/) || test_subdir.match(/omp/) || test_subdir.match(/ampi/)
          'MPI'
        # elsif 
        #   $stderr.puts "runtime is omp ********************************************"
        #   'OMP'
        else
          raise "Unabled to determine runtime based on test subdir. Looking for 'pure' or 'baseline'. Consider renaming test dir."
        end
      end
    end

    def each_config
      if numa_schemes.empty? || payload_counts.empty?
        raise "numa_schemes and payload counts must both have at least one entry. It's possible that some numa schemes (e.g., bind_alternating) are getting eliminated due to other configuration options (e.g., threadmaps only work with sequential numa modes). numa_schemes: #{numa_schemes}  payload_counts: #{payload_counts}"
        end

      # IMPORTANT: keep this in sync with num_configs
      numa_schemes.each do |numa_scheme|
        payload_counts.each do |payload_count|
          process_channel_versions.each do |pcv|
            pcv_chunks = is_work_stealing_channel?(pcv) ? process_channel_chunks : [NA_PCV_CHUNKS]
            pcv_chunks.each do |pcv_num_chunks|
              pc_buf_msg_sizes.each do |pc_buf_msg_size|
                helper_threads_options.each do |use_helpers_if_free_cores|
                  jemalloc_options.each do |use_jemalloc|
                    (max_buffered_chan_bytes_opts || [nil]).each do |max_buffered_chan_bytes|
                      # create a string of environment variable assignments which is to be used as a prefix of running make
                      # commands
                      env_var_assignments = []
                      if total_threads
                        env_var_assignments << "TOTAL_THREADS=#{total_threads}"
                        end # total_threads can be nil
                      env_var_assignments << "THREADS_PER_NODE_LIMIT=#{threads_per_node_max}"
                      env_var_assignments << "THREAD_MAP_FILENAME=#{thread_map_filename}"
                      env_var_assignments << "NUM_ITERATIONS=#{message_count}"
                      env_var_assignments << "PAYLOAD_COUNT=#{payload_count}"
                      env_var_assignments << "NUMA_SCHEME=#{numa_scheme}"
                      env_var_assignments << "CLASS=#{klass}"
                      env_var_assignments << "TOPO=#{topology}"
                      if max_buffered_chan_bytes
                        env_var_assignments << "BUFFERED_CHAN_MAX_PAYLOAD_BYTES=#{max_buffered_chan_bytes}"
                        end

                      # numa_ed = numa_scheme == NA_NUMA_SCHEME ? extra_description : extra_description + '-' + numa_scheme
                      config_extra_description = create_config_extra_description(numa_scheme, pcv_num_chunks, use_jemalloc, use_helpers_if_free_cores, max_buffered_chan_bytes)
                      env_var_assignments << "EXTRA_DESCRIPTION=#{config_extra_description}"

                      env_var_assignments << "PROCESS_CHANNEL_VERSION=#{pcv}"
                      env_var_assignments << "PROCESS_CHANNEL_BUFFERED_MSG_SIZE=#{pc_buf_msg_size}"
                      if is_work_stealing_channel?(pcv)
                        env_var_assignments << "PCV_4_NUM_CONT_CHUNKS=#{pcv_num_chunks}"
                        end
                      if run_args_custom
                        env_var_assignments << "RUN_ARGS='#{run_args_custom}'"
                        end

                      # TODO: possibly make this only true if
                      env_var_assignments << "USE_HELPER_THREADS_IF_FREE_CORES=#{use_helpers_if_free_cores ? 1 : 0} "

                      env_var_assignments << "USE_JEMALLOC=#{use_jemalloc ? 1 : 0}"
                      env_var_assignments << "USE_ASMLIB=#{use_asmlib ? 1 : 0}"

                      env_var_assignments << "OMP_NUM_THREADS=#{omp_num_threads}"

                      if extra_set_env_vars
                        env_var_assignments << extra_set_env_vars
                        end
                      if extra_set_env_vars
                        print "  extra_set_env_vars is #{extra_set_env_vars}".yellow
                        end
                      if config_extra_description
                        puts " | config_extra_description is #{config_extra_description}".cyan
                        end

                      yield(env_var_assignments.join(' '), total_threads, threads_per_node_max, override_runtime, pre_run_cmd, post_run_cmd, omp_num_threads)
                    end
                  end
                end
              end
            end
          end
        end
      end
    end

    def num_configs
      # TODO: verify with test and manually

      # IMPORTANT: keep this in sync with each_config
      runs_per_config * numa_schemes.size * payload_counts.size
    end

    def self.multi_set_num_configs(sets)
      sets.each do |s|
        raise 'set must be of type RunSet' unless s.is_a?(RunSet)
      end
      sets.map(&:num_configs).inject(:+)
    end

    private def validate_value(name_sym, v)
      if ARRAY_FIELDS.include?(name_sym) && !v.is_a?(Array)
        raise "Parameter #{name_sym} must be an array"
        end
      if !ARRAY_FIELDS.include?(name_sym) && v.is_a?(Enumerable)
        raise "Parameter #{name_sym} must not be an enumerable"
      end

      if BOOLEAN_FIELDS.include?(name_sym)
        if v.any? { |elt| !(elt == true || elt == false) }
          raise "Parameter #{name_sym} must be true or false but it is #{v}"
          end
      end
    end
  end # end RunSet class

  class CommandGroup
    attr_reader :env_vars, :binary_affecting_env_vars, :runtime, :benchdir, :total_threads, :threads_per_node_max_for_set,
                :pre_run_cmd, :post_run_cmd, :omp_num_threads

    def initialize(env_vars, binary_affecting_env_vars, runtime, benchdir, total_threads, threads_per_node_max_for_set,
                   pre_run_cmd, post_run_cmd, omp_num_threads)
      @env_vars = env_vars
      @binary_affecting_env_vars = binary_affecting_env_vars
      @runtime = runtime
      @benchdir = benchdir
      @total_threads = total_threads
      @threads_per_node_max_for_set = threads_per_node_max_for_set
      @pre_run_cmd = pre_run_cmd
      @post_run_cmd = post_run_cmd
      @omp_num_threads = omp_num_threads
    end

    def to_s
      fields = %i[env_vars binary_affecting_env_vars runtime benchdir total_threads threads_per_node_max_for_set
                  pre_run_cmd post_run_cmd omp_num_threads]
      s = []
      fields.each do |f|
        s << "#{f}: \t#{send(f)}"
      end
      s.join("\n")
    end
  end

  def slurm_job_state(jobid)
    max_retries = 10
    num_tries   = 0
    delay_secs  = 10

    loop do
      cmd = "squeue -j #{jobid} --Format=state -h"
      stdout, status = Open3.capture2(cmd)
      if status.success?

        o = stdout.strip
        if o.nil? || o.empty?
          return nil
        else
          case o
          when /PENDING/
            return o.cyan
          when /RUNNING/
            return o.yellow
          when /COMPLET/
            return o.green
          else
            return o.purple
          end
        end
      else
        num_tries += 1
        if num_tries == max_retries
          raise "ERROR running command '#{cmd}'".red
        else
          warn "Retrying slurm_job_state command (try #{num_tries}/#{max_retries}) after sleeping #{delay_secs} seconds...".gray
          sleep delay_secs
          delay_secs *= 2
        end
      end
    end
  end

  def nodes_needed(command_group, use_hyperthreads, threads_per_node_max, omp_num_threads)
    nodes, = MachineHelpers.node_config(command_group.runtime, command_group.total_threads, use_hyperthreads, threads_per_node_max, nil, :array, omp_num_threads)
    nodes
  end

  # loops through all command groups and finds the max number of nodes needed
  def max_nodes_needed(command_groups, use_hyperthreads)
    curr_max = -1
    command_groups.each do |cg|
      nodes = nodes_needed(cg, use_hyperthreads, cg.threads_per_node_max_for_set, cg.omp_num_threads)
      curr_max = nodes if nodes > curr_max
    end
    curr_max
  end

  # pass wait_hours as nil if you want to wait forever
  def switches_needed_option(use_hyperthreads, wait_hours = 48)
    # as per https://docs.nersc.gov/jobs/best-practices/#network-locality
    switches = (max_nodes_needed(@@command_groups, use_hyperthreads).to_f / 255.0).ceil
    if (switches > 3)
      $stderr.puts "Not constraining switches as we would need too many and that seems to be problematic on cori (technically need #{switches}).".yellow
      return ''
    end
    raise "super large number of switches needed -- #{switches} sanitcyh check" if switches > 20 # "we should never need more than 3!"
    $stderr.puts "Requesting #{switches} switch(es)... max nodes needed is #{max_nodes_needed(@@command_groups, use_hyperthreads)}".yellow
    cmd = "--switches=#{switches}"
    if(!wait_hours.nil?)
      minutes = wait_hours * 60
      cmd += "@#{minutes}"
    end

    return cmd
  end

  def powers_of_two(start_val, end_val=start_val)
    a = []

    curr = start_val
    while curr <= end_val
      a << curr
      curr *= 2
    end
    a
  end

  # boolean description
  def gen_description(flag, label)
    '-' + label + '-' + (flag == 1 || flag == true ? 'Y' : 'N')
  end

  def gen_val_description(val, label, num_format = nil, digits = nil)
    formatted_val = if num_format && digits
                      format("%0.#{digits}#{num_format}", val)
                    else
                      val.to_s
                    end

    '-' + label + '-' + formatted_val
  end

  def is_work_stealing_channel?(v)
    v.between?(460, 499)
  end

  #     def run_cmd_with_streaming_output(cmd:, dryrun: false, command_logfile: nil, exit_on_error: true)
  #
  #         # log all commands to @command_logfile_f if it has been initialized
  #         # if command fails, the command WILL be in the command logfile.
  #         command_logfile.puts cmd + "\n" if command_logfile
  #         all_stdout = []
  #         status = nil
  #
  #         if !dryrun
  #             begin
  #                 status = PTY.spawn( cmd ) do |stdout, stdin, pid|
  #                     begin
  #                         # indent output of shell a bit (TODO: improve formatting.)
  #                         # TODO: what if it takes a long time for stdout to produce next output; how does this
  #                         #  know it's done? I think this is a bug.
  #                         stdout.each do |line|
  #                             print "  #{line}"
  #                             all_stdout << line
  #                         end
  #                     rescue Errno::EIO
  #                        $stderr.puts "Warning: Errno:EIO error when running command '#{cmd}', but this probably just means " +
  #                            "that the process has finished giving output"
  #                     end
  #                     Process.wait(pid)
  #                 end # ends PTY.spawn
  #
  #                 puts "STATUS of command is #{$?.exitstatus} from shell  and #{status} from PTY.spawn"
  #
  #                 status = $?.exitstatus
  #                 if $?.exitstatus != 0
  #
  #                     err_msg = "\n\nERROR: command '#{cmd}' exited with status code #{status}.\n\nExiting prematurely."
  #                     command_logfile.puts err_msg if command_logfile
  #
  #                     if exit_on_error
  #                         raise err_msg.red
  #                     else
  #                         command_logfile.puts "Hit error message but continuing anyway."
  #                     end
  #                 end
  #             rescue PTY::ChildExited
  #                 puts "The child process exited!"
  #             end
  #         end # not dryrun
  #         return status, all_stdout.join("\n")
  #     end
  #
  # =
  # for running multiple commands in parallel using subprocesses
  def gen_cmd_bash_pipeline(commands)
    # commands shouldn't already have a & in it nor a wait
    commands.each do |c|
      raise "wait found in command #{c}" if c.match /wait/
      raise "& found in command #{c}" if c.match /&\s?$/
    end

    parts = []

    commands.each_with_index do |c, i|
      parts << "#{c} &"
      parts << "pids[#{i}]=$!"
    end

    parts << 'for pid in ${pids[*]}; do'
    parts << '  wait $pid'
    parts << 'done'

    cmd = parts.join("\n")
    cmd
  end

  def run_cmd_with_streaming_output(cmd:, dryrun: false, command_logfile: nil, exit_on_error: true)
    return if dryrun

    # this works if cmd is an array of commands or just a single command. If it's an array of commands, they are run as subproccesses and all commands must finish before this returns.
    is_multiple_cmds = cmd.is_a?(Array) && cmd.size > 1
    all_commands = is_multiple_cmds ? cmd.join("\n") : cmd

    command_logfile&.puts all_commands
    puts all_commands

    stdout_err, status = Open3.capture2e(all_commands)
    command_logfile&.puts stdout_err
    puts stdout_err

    unless status.success?
      puts "\n***********************************************************************\nERROR in running command. Exited with #{status} (see output above):\n".red
      File.open('COMMAND_ERROR_FLAG', 'w') do |f|
        f.puts "cmd failed: #{cmd}"
        f.puts "Output was:\n#{stdout_err}"
      end
      raise 'ERROR' if exit_on_error
    end
  end

  def kill_pids!(pids)
    pids.map { |pid| Process.kill('HUP', pid) }
  end

  #
  # https://stackoverflow.com/questions/356100/how-to-wait-in-bash-for-several-subprocesses-to-finish-and-return-exit-code-0
  #
  #
  #
  #     def run_cmd_with_streaming_output(cmd:, dryrun: false, command_logfile: nil, exit_on_error: true)
  #
  #         # log all commands to @command_logfile_f if it has been initialized
  #         # if command fails, the command WILL be in the command logfile.
  #         command_logfile.puts cmd + "\n" if command_logfile
  #         all_stdout = []
  #         status = nil
  #
  #         if !cmd.is_a?(Array)
  #             cmd = [cmd]
  #         end
  #         do_wait = cmd.size > 1
  #         FileUtils.mkdir_p("temp")
  #
  #         if !dryrun
  #             cmd.each_with_index do |cmd, idx|
  #
  #                 cmd = cmd + " ; echo $? > temp/status_#{idx}_#{Time.now}.log &"
  #                 puts cmd
  #                 stdout_err, status = Open3.capture2e(cmd)
  #                 puts stdout_err
  #                 command_logfile.puts stdout_err if command_logfile
  #
  #                 if(do_wait)
  #                     cmd = "/bin/bash -c wait #{status.pid} || exit -2"
  #                     # cmd = "exit 1;"
  #                     puts cmd
  #                     stdout_err, wait_status = Open3.capture2e(cmd)
  #                     puts stdout_err
  #                     puts  "   wait STATUS is #{wait_status}".yellow
  #
  #                     if(!wait_status.success?)
  #                         puts "ERROR in running command (see output above):".red
  #                         raise "ERROR" if exit_on_error
  #                     end
  #                 else
  #                     if(!status.success?)
  #                         puts "ERROR in running command (see output above):".red
  #                         raise "ERROR" if exit_on_error
  #                     end
  #                 end
  #             end
  #         end
  #     end

  def project_path(file_path, validate_existance = true)
    p = File.join(ENV['CPL'], file_path)
    if validate_existance
      unless File.exist?(p)
        raise "Tried to create a path to file #{p}, but it doesn't exist"
        end
    end
    p
  end

  def env_var_encapsulate(str, var)
    str.gsub(ENV[var], "$#{var}")
  end

  def send_email(subject, msg)
    # use sendmail on perlmutter
    # return if MachineHelpers.perlmutter_compute_node?

    # no mail for perlmutter for now -- see if we get a response from nersc ticket
    return if MachineHelpers.running_on_perlmutter?

    File.open('.temp_email', 'w') do |f|
      f.puts "Subject: #{subject}"
      f.puts msg
    end

    if MachineHelpers.running_on_perlmutter? 
      cmd = "/usr/sbin/sendmail jpsota@nersc.gov < .temp_email"
      run_cmd_with_streaming_output(cmd: cmd)
    end
  end

  def pretty_time_distance(start_time, end_time = nil)
    minutes = if end_time
                # compute distance
                (end_time - start_time) / 60.0 # the time math will return seconds
              else
                # just return number of seconds
                start_time / 60.0
              end
    "#{minutes.round(1)} minutes"
  end

  def slurm_time(min)
    seconds = 0
    minutes = min % 60
    hours = min / 60

    format('%02d', hours.to_s) + ':' + format('%02d', minutes.to_s) + ':' + format('%02d', seconds.to_s)
  end

  def jobs_done(job_dir)
    json_dir = File.join(job_dir, 'json/*/')
    # we need to subtract one, as ./ is returned by find
    `find #{json_dir} -type d -not -empty | wc -l`.to_i - 1
  end

  def json_files_created(job_dir)
    glob = File.join(job_dir, 'json/*/*/*.json')
    cmd = "ls -l #{glob} | wc -l"

    tries = 5
    (0...tries).each do |the_try|
      stdout_err, status = Open3.capture2e(cmd)
      if the_try >= tries && !status.success?
        unless status.success?
          raise "After #{tries} tries, unable to determine the number of json files with command #{cmd}"
          end
      end

      if status.success?
        return stdout_err.to_i
      else
        puts "Failed to determine number of json files -- output was #{stdout_err}".yellow
      end

      # wait a bit before trying again
      sleep 5
    end

    raise "After #{tries} tries, unable to determine the number of json files with command #{cmd}"
  end

  def get_pid
    `echo $$`&.chomp&.to_i
  end

  def hostname
    `hostname`&.chomp
  end

  def self.all_numa_configs
    %w[hyperthread_siblings shared_l3 different_numa none].freeze
  end

  def self.numa_schemes_for_config(runtime, all_schemes, use_threadmap)
    if runtime == 'PureProcPerNUMA'
      if use_threadmap
        all_schemes.grep(/sequence/)
      else
        all_schemes.grep_v('none')
      end
    else
      if use_threadmap
        all_schemes.grep(/sequence/)
      else
        all_schemes
      end
    end
  end

  def append_command_group(all_env_vars, binary_affecting_env_vars, runtime, benchdir, total_threads, threads_per_node_max_for_set, pre_run_cmd, post_run_cmd, omp_num_threads)
    @@command_groups << CommandGroup.new(all_env_vars, binary_affecting_env_vars, runtime, benchdir, total_threads, threads_per_node_max_for_set, pre_run_cmd, post_run_cmd, omp_num_threads)
  end

  def set_terminal_title(title)
    cmd = "echo -n -e \"\033]0;#{title}\007\""
    system(cmd)
  end

  def verify_cori_arch!(arch)
    return unless MachineHelpers.running_on_nersc?
    return if MachineHelpers.running_on_perlmutter?

    unless %w[craype-mic-knl craype-haswell].include?(arch)
      raise "cori_arch must be either craype-mic-knl or craype-haswell, but it is #{arch}"
      end

    cmd = "/bin/bash -c 'module list'"
    stdout_err, status = Open3.capture2e(cmd)
    raise "unable to run command #{cmd}" unless status.success?

    matcher = stdout_err.match /#{arch}/
    unless matcher
      raise "Unable to find loaded module #{arch}. You may need to run knl or haswell at the command line before running this."
      end
  end

  def verify_test_makefile!(benchdir)
    cmd = "make -C #{benchdir} vars"
    stdout_err, status = Open3.capture2e(cmd)
    raise "unable to run command #{cmd}" unless status.success?

    vars = stdout_err

    if vars.match /for nonpure applications/
      # nonpure -- skip this
      puts "Skipping Makefile verification for nonpure testdir: #{benchdir}"
      return
    end

    # make sure certain variables are not set in the test makefile
    msgs = []
    fields = %w[OVERRIDE_RUNTIME THREAD_MAP_FILENAME PURE_MAX_HELPER_THREADS THREADS_PER_NODE_LIMIT]
    fields.each do |f|
      unless vars.match(/\s*#{f}:\s*$/)
        msgs << "* Application Makefile field #{f} must be unset for this benchmark driver to run"
        end
    end

    unless msgs.empty?
      warn warn msgs.join("\n").red
      raise "Makefile in #{benchdir} must be fixed -- see above for details."
    end
  end

  private def slurm_constraint(cori_arch)
    case cori_arch
    when 'craype-haswell'
      'haswell'
    when 'craype-mic-knl'
      'knl'
    when 'x86-milan'
      'cpu'
    else
      raise "Invalid cori arch #{cori_arch}"
    end
  end

  private def linecount(filename)
    `wc -l "#{filename}"`.strip.split(' ')[0].to_i
  end

  private def minutes_remaining(start_batch_time, percent_complete)
    seconds_elapsed = Time.now - start_batch_time
    seconds_per_percent = seconds_elapsed / percent_complete.to_f
    seconds_for_remaining = seconds_per_percent * (100 - percent_complete)
    return (seconds_for_remaining / 60.to_f).round(1)
  end


  # get force_build_prefix from the desired sbatch.sh and look for PREPEND_HOSTNAME_AND_EXTRA_PREFIX
  # ANALYISIS: to just run analysis: three things: set the force* args to the directory, and driver style to analysis
  def run_sets!(sets:, runs_per_config:, test_root_dir:, cori_arch:, max_per_task_minutes:,
                analyze_results_r_script: nil, extra_r_script_args: nil, web_report_comment: nil, dryrun: false,
                collect_perf_events: false, exit_on_run_error: true, run_mode: :release, slurm_partition: :auto,
                extra_env_vars_all_sets: '', start_with_unique_config: 0, driver_style: :interactive, min_nodes_requested: 1,
                use_hyperthreads: true,  add_files_to_git: false, 
                allow_nonzero_exit: false, hours_to_wait_for_switches: nil, slurm_res: nil, allow_rank_0_only_reporting: false,
                run_jobs_in_parallel: true, 
                force_build_prefix: nil, force_outdir: nil, force_all_dirs: nil)

    if collect_perf_events && (ENV['OS'] != 'cag')
      warn 'collect_perf_events set, but can only do this on cag machines at this point. Ignoring.'.yellow
      collect_perf_events = false
    end

    # no slashes in test_root_dir
    raise 'No sets were passed to run_sets!'.red if sets.empty?
    raise "Invalid test_root_dir: #{test_root_dir}" if test_root_dir.match('/')

    unless %i[interactive batch analysis analysis_latest].include?(driver_style)
      raise "Invalid driver style #{driver_style}"
    end

    # handle :analysis_latest driver mode
    if driver_style == :analysis_latest || ! force_all_dirs.nil?

      if(driver_style == :analysis_latest && force_all_dirs != nil)
        raise "You can't have force_all_dirs set in analysis_latest mode"
      end

      if ($0.match 'driver_ran.rb').nil?
        raise "You are running in analysis_latest mode; you MUST run driver_ran.rb from the root benchmark directory to ensure the same driver is used. Do not run with driver.rb. You also have to add ../../ to the benchmark_helpers path at the top of driver_ran.rb."
      end

      # check no failed jobs
      fj = File.join('latest_run', 'failed_jobs.log')
      if (File.exist?(fj) && linecount(fj) > 0)
        raise "failed_jobs.log is non-empty. fix errors."
      end

      if(driver_style == :analysis_latest)
        force_outdir = File.readlink("latest_run").split('/').last
        puts "Analyzing #{force_outdir} dir in analysis_latest mode..."
        matcher = File.read(File.join("latest_run", 'sbatch.sh')).match /PREPEND_HOSTNAME_AND_EXTRA_PREFIX=(\S*)\s/
        force_build_prefix = matcher[1]
      end

      if ! force_all_dirs.nil?
        force_outdir = force_all_dirs
        force_build_prefix = force_all_dirs
      end

    end

    do_rebuild = force_build_prefix.nil?

    if !do_rebuild && driver_style == :interactive
      raise "do_rebuild set to #{do_rebuild} but that option is only available for driver_style: batch and analysis"
      end
    unless %i[auto regular debug scavenger].include?(slurm_partition)
      raise "Invalid slurm partition #{slurm_partition}"
    end

    if driver_style == :analysis && force_build_prefix.nil?
      raise 'Driver style is :analysis (only do CSV collection and data analysis) but force_build_prefix is nil'
    end
    if driver_style == :analysis && force_outdir.nil?
      raise 'Driver style is :analysis (only do CSV collection and data analysis) but force_outdir is not set'
    end

    verify_cori_arch!(cori_arch)
    set_terminal_title('DRIVER: ' + test_root_dir)
    starting_dir = Dir.pwd
    @@command_groups ||= []

    screen_name = ENV['STY']
    subject = "Profiling of #{test_root_dir} running on #{hostname} " + (screen_name ? "(screen #{screen_name})" : '(no screen)')
    send_email(subject, subject + ". Started at #{Time.now}.")

    if force_outdir
      puts "** WARNING: running with forced timesteamp #{force_outdir}\n".yellow
      full_run_timestamp = force_outdir
    else
      full_run_timestamp = `date +"%Y-%m-%d-%H-%M-%S"`&.chomp
    end

    base_out_dir = File.expand_path('runs')
    timestamped_out_dir = File.join(base_out_dir, full_run_timestamp)
    if force_outdir.nil? && Dir.exist?(timestamped_out_dir)
      raise "timestamped_out_dir has already been created: #{timestamped_out_dir}".red
    end

    FileUtils.mkdir_p timestamped_out_dir
    total_start_time = Time.now

    link_name = 'latest_run'
    FileUtils.rm_f(link_name)
    FileUtils.ln_s(timestamped_out_dir, link_name, force: true)
    
    # at top level for convenience
    top_link_name = File.join(ENV['CPL'], link_name)
    FileUtils.rm_f(top_link_name)
    FileUtils.ln_s(timestamped_out_dir, top_link_name, force: true)


    jobname = "#{test_root_dir}-#{full_run_timestamp}"
    csv_filename = 'results.csv'
    csv_full_path = File.join(timestamped_out_dir, csv_filename)

    driver_log = File.join(timestamped_out_dir, 'driver.log')
    driver_log_f = File.open(driver_log, 'w')
    driver_log_f.sync = true

    command_logfile = File.open(File.join(timestamped_out_dir, 'commands.log'), 'w')
    command_logfile.sync = true

    driver_pid = get_pid
    run_unique_id = 0 # global id across all sets

    # the CONFIG_UNIQUE_ID is the same for all runs of a given configuration (e.g., same topology, number of threads, problem size, etc.)
    # this is useful for computing, for example, the median runtime across a set of runs of the same configuration.
    # PREPEND_HOSTNAME_AND_EXTRA_PREFIX prepends the hostname and driver_pid to the build directory
    config_unique_id = 0
    prepend_prefix = force_build_prefix || (hostname + '-' + driver_pid.to_s)
    warn "PREPEND PREFIX:  #{prepend_prefix}".yellow
    base_env_vars = "ASAN=0 TSAN=0 MSAN=0 UBSAN=0 VALGRIND_MODE=0 ENABLE_THREAD_LOGGER=0 PRINT_PROCESS_CHANNEL_STATS=0 DO_PRINT_CONT_DEBUG_INFO=0 COLLECT_THREAD_TIMELINE_DETAIL=0 TRACE_MPI_CALLS=0 TRACE_COMM=0 PREPEND_HOSTNAME_AND_EXTRA_PREFIX=#{prepend_prefix} NO_SLURM_FILE_LOG=0 USE_TRACEANALYZER= DO_JEMALLOC_PROFILE=0 MAP_PROFILE=0"

    # TODO: consider changing this to not exclude the head node as we are now compiling in advance as per https://mail.google.com/mail/u/0/#search/nersc/FMfcgxvzLXGPHVwQnmmRLqswWFBrlrXt
    # base_env_vars   += " NO_HEAD_NODE_COMPUTE=1" if driver_style == :batch

    unless run_mode == :release || run_mode == :debug || run_mode == :profile
      raise 'Invalid run mode. Must be either :release or :debug or :profile'
    end

    debug_flag = run_mode == :debug ? 'DEBUG=1' : 'DEBUG=0'
    profile_flag = run_mode == :profile ? 'PROFILE=1' : 'PROFILE=0'
    release_env_vars = "#{debug_flag} #{profile_flag} " + base_env_vars
   
    # deprecated var
    profile_env_vars = "#{debug_flag} PROFILE=1 " + base_env_vars

    json_stats_base_dir = File.join(timestamped_out_dir, 'json')
    release_json_stats_base_dir = File.join(json_stats_base_dir, run_mode.to_s)

    # create collection of commands (but don't run them yet)
    sets.each do |set|
      benchdir = project_path("test/#{set.test_subdir}")
      set.each_config do |run_env_var_assignments, total_threads, threads_per_node_max_for_set, override_runtime, pre_run_cmd, post_run_cmd, omp_num_threads|
        if config_unique_id >= start_with_unique_config
          runs_per_config.times do |run_number_per_config|
            command_group = []
            cmd = "cd #{benchdir}"
            command_group << cmd

            #############################################
            run_specific_env_vars = run_env_var_assignments

            # add in NPROCS and PURE_RT_NUM_THREADS, etc.
            node_env_vars = MachineHelpers.node_config(set.runtime, total_threads, use_hyperthreads, threads_per_node_max_for_set, nil, :env_vars, omp_num_threads)
            puts "NODE_ENV_VARS (total_threads=#{total_threads}: #{set.run_args_custom}".gray + " #{set.runtime}".yellow + "\t--> #{node_env_vars}"
            puts "\t#{run_env_var_assignments}".gray
            run_specific_env_vars += " #{node_env_vars} "
            run_specific_env_vars += " RUN_OUTPUT_FILENAME=logs/profile_script_run-#{driver_pid}.log"
            run_specific_env_vars += " PURE_RUN_UNIQUE_ID=#{run_unique_id}"

            config_id_env          = " CONFIG_UNIQUE_ID=#{config_unique_id}"
            run_specific_env_vars += config_id_env
            run_specific_env_vars += " RUN_NUM_FOR_CONFIG=#{run_number_per_config}"
            run_specific_env_vars += " ENABLE_HYPERTHREADS=#{use_hyperthreads ? 1 : 0} "
            if override_runtime
              runtime_override = " OVERRIDE_RUNTIME=#{override_runtime}"
            end

            # configure PERF_STAT_OUTFILE
            perf_stat_dir = File.join(json_stats_base_dir, 'profile', run_unique_id.to_s)
            perf_stat_file = File.join(perf_stat_dir, 'perf_stat.csv')
            run_specific_env_vars += " PERF_STAT_OUTFILE=#{collect_perf_events ? perf_stat_file : ''}"

            unique_json_stats_full_dir = File.join(release_json_stats_base_dir, run_unique_id.to_s)
            run_specific_env_vars += " JSON_STATS_DIR=#{unique_json_stats_full_dir}"
            FileUtils.mkdir_p(unique_json_stats_full_dir)

            # WARNING! These are really tricky to get right.
            all_release_env_vars = "#{release_env_vars} #{run_specific_env_vars} #{runtime_override} #{extra_env_vars_all_sets}"
            binary_affecting_env_vars = "#{release_env_vars} #{run_env_var_assignments} #{runtime_override} #{extra_env_vars_all_sets}"

            run_unique_id += 1

            # add this single command group run to the full set of commands to be run
            append_command_group(all_release_env_vars, binary_affecting_env_vars, set.runtime, benchdir, total_threads, threads_per_node_max_for_set, pre_run_cmd, post_run_cmd, omp_num_threads)
          end # ends runs per config

        else
          puts "Skipping config #{config_unique_id} because start_with_unique_config option set to #{start_with_unique_config}".light_magenta
        end

        # now that we're done with runs_per_config runs (all runs for that config), move onto the next config_unique_id
        config_unique_id += 1
      end
    end

    # verify makefile
    unique_run_directories = @@command_groups.map(&:benchdir).uniq
    unique_run_directories.each do |dir|
      verify_test_makefile!(dir)
    end

    # verify no duplicate sets (as defined by runtime config)
    # this is just a sanity check to make sure that

    # this is the uniquing code in the R scripts - keep consistent
    # df$runtime_config <- paste0(
    #     df$runtime,
    #     "-p", df$procs,
    #     "t", df$threads)

    # df <- mutate(df, pcv_config = ifelse(!is.na(process_channel_version), paste("pcv", process_channel_version, sep=''), NA))
    # df <- mutate(df, pcv_config = ifelse(!is.na(pc_buffered_msg_size), paste(pcv_config, "-qs", sprintf("%04d", pc_buffered_msg_size), sep=''), pcv_config))

    # df <- df %>% mutate(runtime_config = ifelse(is.na(pcv_config), runtime_config, paste0(runtime_config, '-', pcv_config)))
    # # put extra_desc at end
    # df <- df %>% mutate(runtime_config = paste0(runtime_config, '-', extra_description))
    runtime_configs = []
    @@command_groups.map do |cg|
      runtime_configs << [cg.runtime, cg.total_threads, cg.threads_per_node_max_for_set, cg.benchdir, cg.binary_affecting_env_vars].join('-')
    end
    # things.group_by { |x| x }.map { |k,v| [k,v.count] }
    dupes = runtime_configs.group_by { |x| x }.map { |k, v| [k, v.count] }

    has_dupes = false
    dupes.map do |d|
      next unless d[1] > runs_per_config

      puts "\nDUPLICATE CONFIGS! #{d[1]} examples of runtime config ".yellow
      puts d[0]
      has_dupes = true
    end

    exit if has_dupes
    puts "No duplicate runtime configurations out of #{runtime_configs.size} (experimental); continuing...".green

    gen_combiner_entry = lambda do 
      msg = []
      msg << "\n--------------------------------------------"
      msg << "DRIVER ENTRY BELOW:\n".yellow
      msg << "'#{full_run_timestamp}', # #{web_report_comment}\n"
      msg << "\n--------------------------------------------"
      return msg.join(' ')
    end
    puts gen_combiner_entry.call

    # do cleanup
    test_dirs = sets.map(&:test_subdir).uniq
    # TODO: figure out how to integrate BENCH_PREFIX and only clean prefix for this particular driver.
    # one idea is to just put the driver host and pid at the beginning of the obj dir and then just clean that
    # then no extra variables are needed....
    if do_rebuild
      # place driver file in that directory
      FileUtils.cp($0, File.join(timestamped_out_dir, "driver_ran.rb"))

      test_dirs.each do |d|
        puts "Cleaning #{d}".cyan
        cmd = "#{base_env_vars} BUILD_EXTRA_PREFIX=#{prepend_prefix} NUMA_SCHEME=none make clean_build_prefix"
        run_cmd_with_streaming_output(cmd: "cd $CPL/test/#{d} && #{cmd}", dryrun: dryrun, command_logfile: command_logfile)
      end
    end

    # 1. Set up master CSV output file
    makefile_path = project_path('support/Makefile_includes/Makefile.extra_targets.mk')
    cmd = "make -f #{makefile_path} experiment_details_header > #{csv_full_path}"
    run_cmd_with_streaming_output(cmd: cmd, dryrun: dryrun, command_logfile: command_logfile)

    # LOGGING
    File.open(File.join(timestamped_out_dir, 'env_vars'), 'w') do |f|
      @@command_groups.each_with_index do |cg, i|
        f.puts "JOB #{i} / #{@@command_groups.size}"
        f.puts "[env_vars]: #{cg.env_vars}"
        f.puts "[binary_affecting_env_vars]: #{cg.binary_affecting_env_vars}"
        f.puts
      end
    end

    if driver_style == :interactive
      puts 'DRIVER MODE: running in INTERACTIVE mode'.green.swap
      runtimes = []

      @@command_groups.each_with_index do |cg, idx|
        puts "-------- JOB #{idx} (#{@@command_groups.size} total) [interactive]".light_magenta
        run_start_time = Time.now # benchmarking

        # first, change to relevant dir
        FileUtils.cd cg.benchdir
        # TODO: if on nersc and have
        # SLURM_JOB_NUM_NODES
        cmd = cg.pre_run_cmd.nil? ? '' : cg.pre_run_cmd + ' && '

       # cmd += "#{cg.env_vars} make vars"
        cmd += "#{cg.env_vars} make run"


        cmd += cg.post_run_cmd.nil? ? '' : ' && ' + cg.post_run_cmd
        # cmd = "#{cg.pre_run_cmd} && #{cg.env_vars} make run && #{cg.post_run_cmd}"
        puts cmd.green
        run_cmd_with_streaming_output(cmd: cmd, dryrun: dryrun, command_logfile: command_logfile, exit_on_error: !allow_nonzero_exit)

        puts "DONE RUNNING MAKE RUN\n\n\n".yellow

        if collect_perf_events
          # TODO: untested as of June 11, 2019
          FileUtils.mkdir_p(perf_stat_dir)
          profile_json_stats_base_dir = File.join(json_stats_base_dir, 'profile')
          profile_json_stats_full_dir = "#{profile_json_stats_base_dir}/#{run_unique_id}"
          # this seems to override previous definition of JSON_STATS_DIR available in cg.env_vars. not sure if later definition will properly override.
          profile_only_env_vars = " JSON_STATS_DIR=#{profile_json_stats_full_dir}"
          cmd = "#{cg.env_vars} #{profile_only_env_vars} make profile-stat"
          puts cmd.cyan
          run_cmd_with_streaming_output(cmd: cmd, dryrun: dryrun, command_logfile: command_logfile)
        end

        # benchmarking / logging
        rt = (Time.now - run_start_time)
        runtimes << rt
        runtime_left = runtimes.mean * (@@command_groups.size - idx + 1)
        msg = "Done JOB #{idx} (#{@@command_groups.size} total) [#{pretty_time_distance(runtime_left)} left]"
        driver_log_f.puts msg
        if run_unique_id % 1000 == 0
          send_email("[#{hostname}] Run update: #{pretty_time_distance(runtime_left)}", msg)
        end
      end

      ############################################################
      # optionally run profile-stat run to collect extra data
      if collect_perf_events
        raise "probably not working"
        if driver_style == :batch
          raise 'Collecting perf events only supported in interactive driver style'
        end

        FileUtils.mkdir_p(perf_stat_dir)
        profile_json_stats_base_dir = File.join(json_stats_base_dir, 'profile')
        profile_json_stats_full_dir = "#{profile_json_stats_base_dir}/#{run_unique_id}"
        profile_only_env_vars = " JSON_STATS_DIR=#{profile_json_stats_full_dir}"
        all_profile_env_vars = profile_env_vars + ' ' + run_specific_env_vars + ' ' + profile_only_env_vars

        cmd = "#{all_profile_env_vars} make profile-stat"
        # run_cmd_with_streaming_output(cmd: cmd, dryrun: dryrun, command_logfile: command_logfile)
        command_group << cmd
        #############################################
      end

    elsif driver_style == :batch
      # at this point the stream of commands has been generated and now must be potentially reordered and issued

      # configure batch job for the largest number of nodes
      max_nodes_needed_calc = max_nodes_needed(@@command_groups, use_hyperthreads)

      # we added this for super large node count on cori
      # if(min_nodes_requested > 2*max_nodes_needed_calc)
      #   min_nodes_requested = max_nodes_needed_calc * 2 
      #   $stderr.puts "Dialing back the number of nodes requested to #{min_nodes_requested} (still enough to do all jobs)..."
      # end

      total_nodes = [min_nodes_requested, max_nodes_needed_calc].max
      parallel_job_factor = (min_nodes_requested.to_f / max_nodes_needed(@@command_groups, use_hyperthreads).to_f).floor

      puts "min nodes req: #{min_nodes_requested}  max_nodes_needed: #{max_nodes_needed(@@command_groups, use_hyperthreads)}  factor: #{parallel_job_factor}"
      raise "parallel job factor invalid: #{parallel_job_factor}. Do you have enough nodes allocated?" if parallel_job_factor == 0

      # figure out total time based on job length and size and fail out if necessary
      total_minutes_to_request = (max_per_task_minutes.to_f * @@command_groups.size.to_f / parallel_job_factor.to_f * 1.1).ceil # add some padding
      if slurm_partition == :debug && total_minutes_to_request > 30
        raise "The debug partition only supports a total of 30 minutes but #{total_minutes_to_request} minutes are needed; switch to the release partition or increase the number of total nodes requested (currently #{total_nodes}). See Cori queue policies at https://docs.nersc.gov/jobs/policy/".red
        end

      if slurm_partition == :auto
        selected_slurm_partition = total_minutes_to_request <= 30 && total_nodes <= 8 ? 'debug' : 'regular'
      else
        selected_slurm_partition = slurm_partition.to_s
      end

      # possibly round up request minutes if debug
      if selected_slurm_partition == 'debug'
        total_minutes_to_request = [30, total_minutes_to_request].max # round up to 30 regardlessly in batch mode
      end
      puts "DRIVER MODE: running in BATCH mode on #{selected_slurm_partition} SLURM queue; will request #{total_minutes_to_request} minutes.".light_magenta.swap

      if selected_slurm_partition == 'regular'
        12.times do |_i|
          print "\aRequesting regular partition :(\n"
          sleep(0.5)
        end
      end

      # 2. Compile all the unique variations
      if do_rebuild
        t0 = Time.now

        # HACK!! rip out RUN_ARGS and EXTRA_DESCRIPTION from the env vars, which really shouldn't be in there.
        build_uniq_command_groups = @@command_groups.map { |cg| [cg.benchdir, cg.binary_affecting_env_vars.gsub(/RUN_ARGS=\'.*\'/, '').gsub(/EXTRA_DESCRIPTION=.*?\s/, '')] }.uniq

        unique_make_commands = build_uniq_command_groups.map do |pair|
          dir = pair[0]
          binary_affecting_env_vars = pair[1]
          "cd #{dir} && #{binary_affecting_env_vars} make"
        end.uniq

        puts 'Unique build commands -- EXPERIMENTAL'.cyan
        ap unique_make_commands

        unique_make_commands.map { |cmd| run_cmd_with_streaming_output(cmd: cmd, dryrun: dryrun, command_logfile: command_logfile) }
        puts "#{unique_make_commands.size} builds took #{(Time.now - t0).to_f / 60.0} minutes.".yellow

        #     # TURNING OFF PARALLEL BUILDS FOR NOW GIVEN COMPILER ISSUES AND OTHER WEIRD THINGS
        #                 # right now there seems to be an issue with running multiple ICC builds in parallel
        #                 max_parallel_jobs = 4  # trying out different values here.
        #
        #                 # unique_make_commands.each_slice(max_parallel_jobs) do |commands|
        #                 #     slice_cmd = commands.join("\n") + "\n wait"
        #                 #     puts "slice command: #{slice_cmd}"
        #                 #     run_cmd_with_streaming_output(cmd: slice_cmd, dryrun: dryrun, command_logfile: command_logfile)
        #                 # end
        #
        #                 jobs_per_slice = unique_make_commands.size / max_parallel_jobs
        #
        #                 # clear this out
        #                 FileUtils.rm_f "COMMAND_ERROR_FLAG"
        #
        #                 pids = []
        #                 slice = 1
        #                 unique_make_commands.each_slice(jobs_per_slice) do |commands|
        #                     # commands is an array of commands to be run serially in the forked subprocess
        #                     pids << Process.fork do
        #                         commands.each_with_index do |cmd, i|
        #                             # TESTING THIS don't exit on error as we need to return here to kill all other processes so we can inspect the error message.
        #                             run_cmd_with_streaming_output(cmd: cmd, dryrun: dryrun, command_logfile: command_logfile, exit_on_error: true)
        #                             puts "SUBPROCESS STATUS: subprocess #{slice}: completed command #{i+1} / #{jobs_per_slice}\n".light_magenta
        #                         end
        #                     end
        #                     slice += 1
        #                 end
        #
        #                 checker_proc = Process.fork do
        #                     # check to see if there's an error with one of the children. If so, kill all of them.
        #                     while(true)
        #                         if File.exist?("COMMAND_ERROR_FLAG")
        #                             puts "One of the children has an error. Killing them all.".red
        #                             kill_pids!(pids)
        #                         end
        #                         sleep(1)
        #                     end
        #                 end
        #
        #                 sleep(2)
        #                 puts "Doing #{unique_make_commands.size} build(s), with up to #{max_parallel_jobs} in parallel (#{jobs_per_slice} builds run serially per subprocess). This will take some time...".yellow
        #
        #                 # now, wait for all of the forked subprocesses and check their exit statuses
        #                 pids.each do |pid|
        #                     pid, status = Process.wait2 pid
        #                     if !status.success?
        #                         kill_pids!(pids)
        #                         kill_pids! [checker_proc]
        #                         raise "subprocess with pid #{pid} failed with status #{status}. Forcibly shut down other processes." unless status.success?
        #                     end
        #                 end
        #
        #                 kill_pids! [checker_proc]
      end

      # make sure all bins are there
      #             puts "Verifying all binaries are there..."
      #             t0 = Time.now
      #             @@command_groups.map do |cg|
      #                 cmd = "cd #{cg.benchdir} && #{cg.binary_affecting_env_vars} make stat_bin"
      #                 run_cmd_with_streaming_output(cmd: cmd)
      #             end
      #             puts "Done verifying binaries (that took #{Time.now - t0}s)".yellow

      # 3. Create the file to submit to SLURM using sbatch
      nersc_batch_script = File.join(timestamped_out_dir, 'sbatch.sh')
      success_msg = "#{jobname} SUCCESSFUL YAY ----- 3434343434342131"
      unless Dir.exist?(json_stats_base_dir)
        raise "didn't create directory #{json_stats_base_dir}"
      end

      # clear out all old batch run dirs
      driver_batch_run_dir = 'driver_batch_run'
      @@command_groups.map(&:benchdir).uniq.each do |benchdir|
        FileUtils.rm_rf(File.join(benchdir, driver_batch_run_dir))
      end

      File.open(nersc_batch_script, 'w') do |f|
        f.puts "#!/bin/bash\n"
        f.puts "\n# sbatch submission script created by #{__FILE__} at #{Time.now}"
        f.puts "# Job timestamp: #{full_run_timestamp}\n"
        f.puts "#SBATCH -N #{total_nodes}"
        #f.puts "#SBATCH --time-min=00:01:00"
        f.puts "#SBATCH -t #{slurm_time(total_minutes_to_request)}"
        f.puts '#SBATCH --mail-user=jimpsota+nersc@gmail.com'
        f.puts '#SBATCH --mail-type=ALL'
        # f.puts '#SBATCH -L cscratch1'
        f.puts "#SBATCH -C #{slurm_constraint(cori_arch)}"
        f.puts "#SBATCH --qos=#{selected_slurm_partition}"

        if(slurm_res.nil?)
        else
          f.puts "#SBATCH --reservation=#{slurm_res}"
          $stderr.puts "Using SLURM reservation #{slurm_res}".yellow
        end

        slurm_job_output_dir = File.join(timestamped_out_dir, 'slurm_job_out')
        FileUtils.mkdir_p(slurm_job_output_dir)
        @available_nodes_before_wait = total_nodes

        @@command_groups.each_with_index do |cg, idx|
          # 1. create the running directory
          run_dir = File.join(cg.benchdir, driver_batch_run_dir, idx.to_s)
          FileUtils.mkdir_p(run_dir)

          # 2. set up the job to run in run_dir
          f.puts "\n### JOB STEP #{idx} (#{@@command_groups.size}) ###"
          f.puts "cd #{run_dir}"
          slurm_output = File.join(slurm_job_output_dir, "job-#{idx}.out")

          f.puts cg.pre_run_cmd if cg.pre_run_cmd
          f.puts "#{cg.env_vars} SLURM_OUTPUT_FILE=#{slurm_output} SLURM_NO_SRUN_JOB_LIMIT=1 BATCH_DRIVER_RUN_DIR=#{run_dir} make -C #{cg.benchdir} raw-background-run #{run_jobs_in_parallel ? '&' : ''}"

          # testing this!
          # f.puts "#{cg.env_vars} SLURM_OUTPUT_FILE=#{slurm_output} SLURM_NO_SRUN_JOB_LIMIT=1 BATCH_DRIVER_RUN_DIR=#{run_dir} make -C #{cg.benchdir} raw-background-run"
          f.puts cg.post_run_cmd if cg.post_run_cmd
        end

        # wait for all jobs -- important!
        f.puts "\n\n# ---------------------------------------------------------- #"
        f.puts 'wait'
        f.puts "echo '#{success_msg}'"
      end # ends file open

      puts "Running #{@@command_groups.size} jobs in batch mode on #{cori_arch}, with build prefix: #{prepend_prefix}; will request #{total_minutes_to_request} minutes.".green

      ########################################
      # now the sbatch script has been crated. Submit it.
      sbatch_output = File.join(timestamped_out_dir, 'sbatch.out')

      # is this --export=ALL an issue?
      cmd = "sbatch --job-name=#{jobname} #{switches_needed_option(use_hyperthreads, hours_to_wait_for_switches)} --export=ALL --output=#{sbatch_output} #{nersc_batch_script}"

      # in case we need to resubmit the job
      File.open(File.join(timestamped_out_dir, 'sbatch_submit.sh'), 'w') do |f|
        f.puts cmd
      end

      chmod_cmd = "chmod +x #{File.join(timestamped_out_dir, '*.sh')}"
      _, status = Open3.capture2(chmod_cmd)
      raise "failed to chmod scripts: #{chmod_cmd}" unless status.success?

      start_batch_time = Time.now
      if dryrun
        warn 'DRYRUN mode -- not actually running job.'
      else
        stdout, status = Open3.capture2(cmd)
        unless status.success?
          raise "sbatch command failed (build prefix was #{prepend_prefix})".red
        end

        matcher = stdout.match /Submitted batch job (\d+)/
        raise unless matcher&.size >= 2

        job_id = matcher[1].to_i

        ###### now wait for all jobs to complete
        total_jobs = @@command_groups.size.to_f

        loop do
          state = slurm_job_state(job_id)
          break if state.nil?

          percent_complete = (jobs_done(timestamped_out_dir) / total_jobs * 100).round(2)
          minutes_left = minutes_remaining(start_batch_time, percent_complete)

          print "[#{Time.now}] #{selected_slurm_partition} partition\tstate: ".gray
          print state.yellow
          print "\t\t\t[#{percent_complete}% done]\t".gray
          print "~#{minutes_left} min left  "
          print "watch_job #{job_id}\t".light_blue
          print "build_prefix: #{prepend_prefix}\n".gray
          sleep 15
        end
        batch_run_time = Time.now - start_batch_time

        ######################################################################################
        # now, make sure the success message is found in the output of the batch job
        stdout, status = Open3.capture2('tail', '-1', sbatch_output)
        unless status.success?
          raise "Wasn't able to tail sbatch output file #{sbatch_output}"
        end

        if stdout.strip != success_msg
          sbatch_output_for_email, = Open3.capture2('tail', '-100', sbatch_output)
          email_msg = "Driver file entry:\n#{web_report_comment}\n"
          email_msg += sbatch_output_for_email
          send_email("FAILED JOB: #{test_root_dir} / full_run_timestamp", "Last bit of log file:\n" + email_msg)
          raise "Didn't find success message in sbatch output file #{sbatch_output} for PREPEND_HOSTNAME_AND_EXTRA_PREFIX = #{prepend_prefix}"
        end

        ############################################
        # Now, all runs are done. Collect the CSVs.
        if(allow_rank_0_only_reporting) 
          expected_num_json_files = @@command_groups.reduce(0) { |sum, cg| sum + 1 }
        else
          expected_num_json_files = @@command_groups.reduce(0) { |sum, cg| sum + cg.total_threads }
        end

        puts "\nWaiting a bit before we parse the JSON files as there seems to be some IO delay (or something).\n".yellow
        puts gen_combiner_entry.call

        if expected_num_json_files > 20000
          sleep(60*15)
        elsif expected_num_json_files > 2000
          sleep(60*5)
        else
          sleep(60*1)
        end

        # also, verify the number of output json files is as expected
        actual_num_json_files = json_files_created(timestamped_out_dir)&.to_i

        # special thing for zero files
        num_tries = 0
        while(num_tries < 60)
          if(actual_num_json_files == 0) 
            puts "    Seeing if we can get more json files... sleeping.".yellow
            sleep(15)
            actual_num_json_files = json_files_created(timestamped_out_dir)&.to_i
            num_tries = num_tries + 1
          else
            break
          end
        end

        # because AMPI allows us to overdecompose, we may have more json files than threads (note: total_threads indicates the number of physical hardware threads used to run)
        if actual_num_json_files < expected_num_json_files
          # if there were only a few remaining, create a log of the failed jobs, and a script to rerun them
          File.open(File.join(timestamped_out_dir, 'failed_jobs.log'), 'w') do |failed_log|
            @@command_groups.each_with_index do |cg, idx|
              json_glob = File.join(release_json_stats_base_dir, '/', idx.to_s, '/', '*.json')
              actual_logs = `ls -l #{json_glob} | wc -l`.to_i

              if actual_logs < cg.total_threads
                failed_log.puts "JOB #{idx} FAILED. Expected #{cg.total_threads} but instead there are #{actual_logs}"
              end

              if actual_logs > cg.total_threads
                $stderr.puts "Warning: found MORE json log files than expected. This can be expected when using AMPI with more virtual processes and hardware processes"
              end

            end
          end
          raise "Expected there to be #{expected_num_json_files} JSON files in json output dir, but there were #{actual_num_json_files}. See failed_jobs.log for the failed jobs (experimental) and create a new sbatch.sh to run just these jobs manually. Then run this driver in analysis only mode.".red
        end
      end
    end

    puts "\nWaiting a bit before we parse the JSON files as there seems to be some IO delay (or something).\n".yellow
    sleep(5)

    max_parallel_jobs = (MachineHelpers.num_cores * 0.8).to_i

    # write each one to a temp file, and then concatenate then together later
    outdir = File.join(timestamped_out_dir, 'temp_csvs')
    FileUtils.mkdir_p(outdir)

    exp_details_start = Time.now
    idx = -1

    # limit max parallel jobs due to shell issue
    max_jobs_for_exp_det = [max_parallel_jobs, 16].min
    puts "Now, generate the experiment details in parallel (up to #{max_jobs_for_exp_det} in parallel)"
    @@command_groups.each_slice(max_jobs_for_exp_det) do |command_groups|
      # fork off a bunch of jobs -- running them in the background and then waiting
      cmds = command_groups.map do |cg|
        idx += 1
        "cd #{cg.benchdir} && #{cg.env_vars} make experiment_details > #{File.join(outdir, "temp_#{idx}.csv")} &"
      end
      cmds << 'wait'

      # known issue -- if any of the above commands fail, this doesn't exit given how wait works. See https://stackoverflow.com/questions/356100/how-to-wait-in-bash-for-several-subprocesses-to-finish-and-return-exit-code-0.
      # command_string = cmds.join("\n")
      # puts "#{idx}: #{command_string}"
      # run these in parallel
      run_cmd_with_streaming_output(cmd: cmds, dryrun: dryrun, command_logfile: command_logfile)
    end
    puts "\nWaiting a bit before we concatenate the temp CSVs as there seems to be some IO delay (or something).\n".yellow
    sleep(10)
    exp_details_total = Time.now - exp_details_start

    # now, concatenate all of them together
    concat_start = Time.now
    cmd = "cat #{outdir}/temp_*.csv >> #{csv_full_path}"
    puts "pwd is #{FileUtils.pwd}.   cmd is #{cmd}".red
    run_cmd_with_streaming_output(cmd: cmd, dryrun: dryrun, command_logfile: command_logfile)
    concat_total = concat_start - Time.now
    ############################################

    driver_log_f.puts "*** Done all #{@@command_groups.size} runs ***"
    driver_log_f.close

    # now, run analytics
    FileUtils.cd timestamped_out_dir # move to out log dir so output files from R script are there as well

    web_url = nil
    if analyze_results_r_script &&
       if File.exist?(analyze_results_r_script)

         cmd = ''
         # this is annoying; the NERSC hosts version of R only works with intel toolchain
         # it also doesn't work with vtune
         if MachineHelpers.running_on_nersc?
           cmd += 'module swap PrgEnv-gnu PrgEnv-intel ; '
          end
         cmd += "#{analyze_results_r_script} #{test_root_dir} #{csv_filename} #{full_run_timestamp} #{runs_per_config} #{extra_r_script_args} ; "
         if MachineHelpers.running_on_nersc?
           cmd += 'module swap PrgEnv-intel PrgEnv-gnu'
          end

         # drop a file in the timestamped_out_dir to allow me to re-run analysis script
         # but here, we just run the command directly (we do not use the script file)
         exe_name = 'analyze_results.bash'
         File.open(exe_name, 'w') do |f|
           # testing this
           f.puts "echo Ensure that failed_jobs is empty"
           f.puts "cat failed_jobs.log"
           f.puts env_var_encapsulate(cmd, 'CPL')
         end
         FileUtils.chmod '+x', exe_name

         # generate web report
         web_script = project_path('support/experiments/html/display_dir.rb').freeze
         web_cmd = "#{web_script} #{test_root_dir} #{timestamped_out_dir}"
         web_cmd += " '#{web_report_comment}'" if web_report_comment

         # drop a file in the timestamped_out_dir to allow me to re-run web reporting script
         exe_name = 'gen_web_report.bash'
         File.open(exe_name, 'w') do |f|
           f.puts env_var_encapsulate(web_cmd, 'CPL')
         end
         FileUtils.chmod '+x', exe_name

         # now make an uber script
         exe_name = 'do_all_reporting.bash'
         File.open(exe_name, 'w') do |f|
           f.puts <<~SCRIPT
             #!/bin/bash
             set -ex
             ./analyze_results.bash
             ./gen_web_report.bash
           SCRIPT
         end
         FileUtils.chmod '+x', exe_name

         # now run the commands now that we've made the scripts

         puts cmd.cyan
         run_cmd_with_streaming_output(cmd: cmd, dryrun: dryrun, command_logfile: command_logfile)

         puts web_cmd.cyan
         run_cmd_with_streaming_output(cmd: web_cmd, dryrun: dryrun, command_logfile: command_logfile)

         unless dryrun
           File.open(File.join(timestamped_out_dir, 'web_url')) do |f|
             web_url = f.read
           end
         end
       else
         raise "Unable to find analysis script #{analyze_results_r_script}"
       end
    end

    email_msg = []
    email_msg << "See attached for output graphs of #{test_root_dir} run #{full_run_timestamp}."
    email_msg << gen_combiner_entry.call
    email_msg << "Also check #{web_url}." if web_url
    email_msg << "\n\nExperiment took #{pretty_time_distance(total_start_time, Time.now)} in total; batch job execution time took #{pretty_time_distance(batch_run_time)}."
    unless dryrun
      send_email("#{test_root_dir} GRAPHS: #{full_run_timestamp}", email_msg.join(' '))
    end

    command_logfile.close

    # add newly-generated files to repository
    if add_files_to_git
      unless BenchmarkHelpers.osx_host?
        cmd = 'git add -f ./*' # already in timestamped_out_dir
        run_cmd_with_streaming_output(cmd: cmd)
        added_to_repo = true
      end
    end

    msg = "  ★ Success running #{@@command_groups.size} jobs, with build prefix #{prepend_prefix}  "
    bar = '━' * msg.length
    spaces = ' ' * msg.length
    puts
    puts " ┏#{bar}┓ ".green
    puts " ┃#{spaces}┃ ".green
    puts " ┃#{msg}┃ ".green
    puts " ┃#{spaces}┃ ".green
    puts " ┗#{bar}┛ ".green
    puts
    puts "  #{nersc_batch_script}" if driver_style == :batch
    puts "  #{sbatch_output}" if driver_style == :batch
    if added_to_repo
      puts '  Output files added to repository; git push to complete the commit.'.light_magenta
    end

    minutes_per_task = (Time.now - total_start_time) / @@command_groups.size / 60.to_f
    puts "  Requested #{max_per_task_minutes} minutes/task; actually took #{minutes_per_task} minutes."
    puts "  Experiment details parsing took #{exp_details_total / 60.to_f} min; csv concat took #{concat_total / 60.to_f} min."
    puts "  #{csv_full_path}"
    puts "\n  #{web_url}\n".cyan if web_url

    set_terminal_title(hostname)

    # go back to starting directory
    FileUtils.cd starting_dir
    @@command_groups = []
  end

  test_runset if $PROGRAM_NAME == __FILE__
end # ends module BenchmarkUtils
