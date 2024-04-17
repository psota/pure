#!/usr/bin/env ruby

# Author: James Psota
# File:   benchmark_utils.rb 

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



require 'fileutils'
require 'json'
require 'time'
require 'trollop'
require 'open3'
require 'shellwords'

require_relative 'db/pure_database_helpers'
include PureDatabase


module BenchmarkUtils

    $stdout.sync = true

    def self.create_status_thread(cmd = nil)
        Thread.new {
            sleep(1)
            i = 0
            char_array = ['-', '\\', '|', '/'].freeze

            max_cmd_len = 120.freeze
            msg = cmd.nil? ? "[running...]" : "[running: #{cmd[0..max_cmd_len].strip}#{cmd.size > max_cmd_len ? '...' : ''}]"
            
            while(true)
                sleep 0.2
                print "\r"
                print "#{char_array[i]} #{msg}".colorize(:light_black) 
                i = (i+1) % char_array.size
            end
        }
    end

    def self.run_and_validate_command2(cmd, opts = {})

        if(opts[:verbose])
            puts "Running '#{cmd}' from #{Dir.pwd}".light_black
            puts "  options are: + #{opts}".light_black
        end

        if(opts[:pre_run_lambda]) 
            opts[:pre_run_lambda].call
        end

        status_thread = create_status_thread(cmd)

        stdout, stderr, status = Open3.capture3(cmd)
        Thread.kill(status_thread)
        print "\r" # reset cursor to beginning of line

        print_output = lambda do |stdout, stderr|
            $stdout.puts "__STDOUT______________________________#{' ' * 110}\n".colorize(:light_magenta) + stdout unless stdout.size == 0
            $stderr.puts "__STDERR______________________________#{' ' * 110}\n".colorize(:light_magenta) + stderr unless stderr.size == 0
        end
        print_output.call(stdout, stderr)

        if(opts[:post_run_lambda]) 
            opts[:post_run_lambda].call
        end

        if( ! status.success? )
            $stderr.puts "☠ ERROR: #{$0} got error code #{status.to_s} from running '#{cmd}'. Exiting.".red 
            raise "FATAL ERROR."
        end

        return [stdout, stderr, status]
    end

    def self.bench_path(bench_name)
        bench_base_dir = File.join(File.dirname(__FILE__), "../../test/")
        bench_path = File.join(bench_base_dir, bench_name)
        raise "Benchmark directory #{bench_path} doesn't exist." unless Dir.exists?(bench_path)

        return bench_path
    end


    class Experiment

        attr_reader :id

        def initialize(name, opts = {})
            @name = name
            @runs = []
            @id   = nil
            @options = opts
        end

        def name_with_id
            "#{@id.to_s.rjust(5, "0")}_#{@name.gsub(/\s/, '_')}"
        end

        def add_run(run)
            @runs << run
        end

        def execute!

            if(@options[:clean])
                clean!
            end

            experiment_metadata = {}
            experiment_metadata[:name] = @name

            # insert new experiment into the database and bind all runs to it via foreign key
            cmd = []
            cmd << "INSERT INTO experiments ("
            cmd << prepare_values(experiment_metadata)
            cmd << ")"
            cmd << "returning id;"

            res = PureDatabase.execute_sql(cmd)

            # set up foreign key
            @id = res[0]['id']

            #### Perform benchmark analysis
            experiment_subdir = "../../experiments/#{name_with_id}"
            experiment_dir = File.join( File.dirname(__FILE__), experiment_subdir )
            FileUtils.mkdir_p(experiment_dir)
            puts "Writing experiment analysis files to #{experiment_dir}"

            #### Execute the benchmark
            @runs.map do |run|
                run_logdir = run.execute!(@id)
                run_name = run_logdir.split('/').last
                FileUtils.ln_s(run_logdir, File.join(experiment_dir, run_name))
            end

            ## generate graphs
            graph_script = File.join( File.dirname(__FILE__), "graph_utils/graph_experiment.R" )
            cmd = "Rscript #{graph_script} #{@id} #{experiment_dir}"
            puts "Graph gen cmd: #{cmd}"
            stdout_str, stderr_str, status = BenchmarkUtils.run_and_validate_command2(cmd)

            # generate webpage with results
            experiment_webpage_script = File.join( File.dirname(__FILE__), "html/experiment.rb" )
            cmd = "#{experiment_webpage_script} #{experiment_dir}"
            puts "Webpage gen cmd: #{cmd}"
            stdout_str, stderr_str, status = BenchmarkUtils.run_and_validate_command2(cmd)

            puts " Completed #{@runs.size == 1 ? 'one run' : "all #{@runs.size} runs"}. See results in #{experiment_subdir}".black.on_green
            puts

        end

        def clean!
            unique_benches = @runs.map{|r| r.bench_name}.uniq

            puts "make cleaning the following runs: " + unique_benches.join(', ')

            unique_benches.each do |b|

                bench_path = BenchmarkUtils.bench_path(b)
                FileUtils.cd bench_path

                clean_cmd = "make clean"
                BenchmarkUtils.run_and_validate_command2(clean_cmd, @options)

            end

        end

    end # class Experiment

    class Run

        attr_reader :bench_name

        def initialize(bench_name, mpi_processes, threads_per_mpi_process, opts = {})
            @bench_name = bench_name
            @mpi_processes = mpi_processes
            @threads_per_mpi_process = threads_per_mpi_process
            @options = opts
        end

        def to_string
            "Run: [#{@bench_name}, #{@mpi_processes}, #{@threads_per_mpi_process}, #{@options}]"
        end


        def execute!(experiment_id)

            bench_path = BenchmarkUtils.bench_path(@bench_name)

            puts "\n> PROCESSING #{bench_path}...".yellow
            FileUtils.cd bench_path

            # create directory for log files. this is a hardcoded path where the benchmark will place
            # its json logs.
            temp_logdir = File.join(bench_path, "runs", "temp_latest")

            # clean out temp_logdir
            FileUtils.rm_rf( temp_logdir )
            FileUtils.mkdir_p temp_logdir
            raise "#{temp_logdir} must be empty to run experiments" unless Dir.glob(File.join(temp_logdir, '*')).size == 0

            debug_flag = @options[:debug] ? "DEBUG=1" : "DEBUG=0"
            trace_comm_flag = @options[:trace_comm] ? "TRACE_COMM=1" : "TRACE_COMM=0"

            nprocs_flag = "PURE_NUM_PROCS=#{@mpi_processes}"
            nthreads_flag = "PURE_RT_NUM_THREADS=#{@threads_per_mpi_process}"

            timestamp = Time.now.strftime("%Y-%m-%d.%H:%M:%S.%3N")
            new_logdir = File.join('runs', timestamp)
            latest_sym = "runs/latest"

            # we pass this into the shelling out routine so it still runs even if the shelled out command fails.
            clean_up_dirs_lambda = lambda do 
                FileUtils.mv temp_logdir, new_logdir
                FileUtils.rm_rf latest_sym 
                FileUtils.ln_s timestamp, latest_sym
            end

            run_command = %Q{#{debug_flag} #{trace_comm_flag} #{nprocs_flag} #{nthreads_flag} #{@options[:env_vars]} make run}
            run_opts = @options.merge({ :post_run_lambda => clean_up_dirs_lambda })
            stdout_str, stderr_str, make_status = BenchmarkUtils.run_and_validate_command2(run_command, run_opts)

            # write ANSI and HTML output to log dir
            write_file_and_html = lambda do |log_dir, ansi_string, filename|
                full_filename = File.join(log_dir, filename)
                File.open(full_filename, 'w') {|f| f.print ansi_string}

                # create the HTML version from the ANSI file
                cmd = "ansi-to-html #{full_filename} &> #{full_filename}.html"
                BenchmarkUtils.run_and_validate_command2(cmd)
            end

            write_file_and_html.call(new_logdir, stdout_str, "stdout.log")
            write_file_and_html.call(new_logdir, stderr_str, "stderr.log")
            File.open(File.join(new_logdir, "command.log"), 'w') {|f| f.print run_command}

            #######################################

            # now, insert an entry into the database for this run.
            json_logs = Dir.glob(File.join(new_logdir, "*.json"))
            raise "ERROR: Unable to find any json logfiles in #{new_logdir}. Exiting..." unless json_logs.size > 0

            expected_num_logs = @mpi_processes * @threads_per_mpi_process
            raise "Expected #{expected_num_logs} log files, but instead found #{json_logs.size}" unless (json_logs.size == expected_num_logs)

            run_metadata = {}
            run_metadata['git_branch']    = ` git rev-parse --abbrev-ref HEAD `.strip
            run_metadata['git_hash']      = ` git rev-parse --verify HEAD `.strip
            run_metadata['directory']     = new_logdir
            run_metadata['run_command']   = run_command
            run_metadata['experiment_id'] = experiment_id
            run_metadata['stderr']        = stderr_str
            run_metadata['stdout']        = stdout_str
            run_metadata['status_code']   = make_status.to_i
            run_metadata['num_procs']     = @mpi_processes
            run_metadata['num_threads']   = @threads_per_mpi_process

            # first, insert this new run
            cmd = []
            cmd << "INSERT INTO runs ("
            cmd << prepare_values(run_metadata)
            cmd << ")"
            cmd << "returning id;"

            res = PureDatabase.execute_sql(cmd)
            run_id = res[0]['id']

            # set up foreign key
            other_thread_metadata = {}
            other_thread_metadata[:run_id] = run_id

            # save this run_id to a file, for use by other systems
            run_id_file = File.join(new_logdir, "run_id")
            File.open(run_id_file, 'w') {|f| f.print run_id}

            #### Insert stats from each thread run into the database
            json_logs.each do |json_log|

                puts "================PARSING JSON FILE: #{json_log}".cyan
                thread_run_hash = JSON.parse(File.open(json_log).read)

                cmd = []
                cmd << "INSERT INTO thread_runs ("
                cmd << prepare_values(thread_run_hash.merge(other_thread_metadata))
                cmd << ");"

                PureDatabase.execute_sql(cmd)

                # TODO: uniqueness on row; don't insert twice
            end

            full_logdir_path = File.join( bench_path, new_logdir)

            ## generate communication visualization
            if(@options[:trace_comm])
                comm_script = File.join( File.dirname(__FILE__), "graph_utils/plot_comm_graph.R" )
                cmd = "Rscript #{comm_script} #{full_logdir_path} #{full_logdir_path} #{@threads_per_mpi_process} #{@mpi_processes}"
                puts "Comm graph gen cmd: #{cmd}"
                stdout_str, stderr_str, status = BenchmarkUtils.run_and_validate_command2(cmd)
            end

            puts
            puts "★★★ SUCCESS: #{self.to_string}.".green
            puts

            return full_logdir_path

        end

    end # class Run

end # BenchmarkRunner