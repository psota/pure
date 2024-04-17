#!/usr/bin/env ruby

# Author: James Psota
# File:   experiment_details.rb 

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



require 'optimist'
require 'json'
require 'awesome_print'
require 'colorize'
require 'fileutils'
require 'byebug'

opts = Optimist::options do
	opt :os, "Operating system", :type=>:string, :required=>true
	opt :runtime_system, "Runtime system", :type=>:string, :required=>true
	opt :config_unique_id, "config-unique-id is the same for all runs of a given configuration " +
	                       "(e.g., same topology, number of threads, problem size, etc.); this is useful for " +
	                       "computing, for example, the median runtime across a set of runs of the same configuration.", 
	                       :type=>:string, :required=>true
	opt :nprocs, "Num procs (use for MPI programs)", :required=>true, :type=>:int
	opt :threads, "Num threads", :type=>:int, :required=>true
	opt :omp_num_threads, "OMP_NUM_THREADS", :type=>:string, :required=>false
	opt :total_threads, "Total number of threads, which may be less than nprocs * threads if TOTAL_THREADS is overridden in application Makefile", :required=>true, :type=>:int
	opt :run_args, "Application arguments", :type=>:string
	opt :git_hash, "Git hash", :type=>:string, :required=>true
	opt :git_branch, "Git branch", :type=>:string, :required=>true
	opt :test_dir, "Absolute path test directory", :type=>:string, :required=>true
	opt :bin_name, "Binary name", :type=>:string, :required=>true
	opt :numactl_desc, "CPU Affinity Scheme Description", :type=>:string, :required=>true
	opt :numactl_cpus, "CPU Affinity CPUs used", :type=>:string, :required=>true
	opt :topology, "Benchmark topology (used for DT, and possibly others)", :type=>:string, :required=>true
	opt :class, "Benchmark class / problem size (used for DT, and possibly others)", :type=>:string, :required=>true
	opt :cxxflags, "CXXFLAGS", :type=>:string, :required=>true
	opt :cflags, "CFLAGS", :type=>:string, :required=>true
	opt :heap_max_total_bytes, "Output of massif_heap_maxes.rb", :type=>:string, :required=>false
	opt :extra_description, "Additional details about the benchmark, to be appended to the runtime system", :type=>:string, :required=>false
	opt :json_stats_dir, "JSON stats directory", :type=>:string, :required=>false
	opt :perf_stats_file, "perf stat file output in semi-colon-separated format", :type=>:string, :required=>false
	opt :process_channel_version, "Process channel version", :type=>:string, :required=>false
	opt :process_channel_buffered_msg_size, "Process channel buffered message size", :type=>:string, :required=>false
	opt :use_jemalloc, "jemalloc", :type=>:string, :required=>false
	opt :use_asmlib, "asmlib (memcpy, etc.)", :type=>:string, :required=>false
	opt :run_num_for_config, "The iteration number of a given run that is run multiple times", :type=>:integer, :required => false
	opt :max_run_args, "Max run_args", :type=>:integer
	opt :rank_0_stats_only, "Only reporting rank 0 json stats only", :type=>:string, :required=>false
end

def parse_intlike_arg(arg)
	return nil if arg.nil? || arg.size == 0
	arg.to_i
end

def clean_bash_garbage_zero_value(arg)
	 arg == "0" ? nil : arg
end

def total_threads_with_omp(omp, tt)
	if (omp)
		tt * omp 
	else
		tt
	end
end

# also update this value in custom_timer_headers.rb
CUSTOM_TIMERS = 28
def get_common_vals(opts)

	vals = []
	# date optimization system runtime procs threads run_args end-to-end_runtime_(ns) git_hash branch test_dir bin_name
	vals << Time.now.strftime("%m/%d/%Y")
	vals << nil # optimization column
	vals << opts[:os]
	vals << opts[:runtime_system]
	vals << opts[:config_unique_id]
	vals << opts[:run_num_for_config]
	vals << opts[:nprocs]
	vals << opts[:threads]
	vals << parse_intlike_arg(opts[:omp_num_threads])

	vals << total_threads_with_omp(parse_intlike_arg(opts[:omp_num_threads]), opts[:total_threads])

	# preprocess process channel options as we often pass them in as blank strings due to limitations in our environment
	# variable approach. 
	vals << parse_intlike_arg(opts[:process_channel_version])
	vals << parse_intlike_arg(opts[:process_channel_buffered_msg_size])

	vals << parse_intlike_arg(opts[:use_jemalloc])
	vals << parse_intlike_arg(opts[:use_asmlib])

	vals << opts[:run_args]
	vals << opts[:git_hash]
	vals << opts[:git_branch]

	# last two sections of test dir
	vals << opts[:test_dir].split('/').last(2).join('/')

	vals << opts[:bin_name]
	
	vals << opts[:numactl_desc]
	# convert 0,16 -> 0 16 as Excel chokes on the comma when importing CSVs
	vals << opts[:numactl_cpus]&.gsub(/,/, ' ')

	vals << opts[:topology]
	vals << opts[:class]

	vals << opts[:cxxflags]
	vals << opts[:cflags]

	# run args, split out into columns
	run_args = opts[:run_args].split(/\s+/)
	raise "Unsuppored number of run_args; must update header row. Currently supports #{opts[:max_run_args]} but is #{run_args.size}" if run_args.size > opts[:max_run_args]
	opts.max_run_args.times do |t|
		# pad out vals with nil up to opts[:max_run_args]
		vals << run_args[t] || nil
	end

	vals << parse_intlike_arg(opts[:heap_max_total_bytes])
	
	# clean out "0" passed as the extra_description, which results from bash not handling empty function arguments well (it converts them to zero)
	vals << clean_bash_garbage_zero_value(opts[:extra_description])

	return vals
end

private def get_thread_specific_vals(json_filename, opts)

	thread_run_hash = JSON.parse(File.open(json_filename).read)
	raise "Unable to load JSON stats file #{json_filename}" unless thread_run_hash.keys.size > 0

	vals = []
	vals << thread_run_hash["pure_thread_rank"]
	vals << thread_run_hash["pure_run_unique_id"]
	vals << thread_run_hash["hostname"]
	vals << thread_run_hash["stats_filename"]

	# configure custom timers
	timer_prefixes = [''] + (0..CUSTOM_TIMERS).to_a.map {|e| "custom#{e}_" }

	timer_prefixes.each do |p|
		# end-to-end timer
		vals << thread_run_hash["#{p}timer_name"] unless p.size == 0 # don't print name for end-to-end special case
		vals << thread_run_hash["#{p}elapsed_cycles"]
	end

	return vals

end

private def get_perf_stats_vals(perf_stats_filename)

	# example file:
	# 96100629;;cycles:pp;19.83%;29441741;61.72;;;;
	# 50410522;;branches;39.82%;30161355;68.64;;;;
	# 227920;;branch-misses;16.12%;34627450;68.50;0.45;of all branches
	# 83520069;;L1-dcache-loads;54.72%;37027111;69.56;;;;
	# 1191277;;L1-dcache-load-misses;13.77%;32259347;62.08;1.43;of all L1-dcache hits
	# 584870;;L1-dcache-store-misses;16.26%;31214325;64.92;;;;
	# 325390;;LLC-loads;17.97%;25057930;55.52;;;;
	# 3554;;page-faults;3.52%;54452344;100.00;;;;
	# 178;;context-switches;2.12%;54452344;100.00;;;;
	# 231264612;;instructions;32.77%;27865963;69.10;2.41;insn per cycle;;;;

	raise "Unable to find perf stats file #{perf_stats_filename}" unless File.exist?(perf_stats_filename)

	vals = []
	File.open(perf_stats_filename) do |f|
		f.each_line do |line|
			sep = ';'  # defined in Makefile.profiling.mk target profile-stat. keep in sync.
			vals << line.split(sep)[0] # first entry is the value
		end
	end

	vals
end


def main(opts)

	if(opts[:json_stats_dir]&.size <= 0 && opts[:perf_stats_file]&.size > 0)
		raise "Unable to use perf-stats-file option without json-stats-file-prefix option. You must use both."
	end

	common_vals = get_common_vals(opts)

	# loop through each run, and generate rows in the output for each run with the common values
	if(opts[:json_stats_dir] && opts[:json_stats_dir].size > 0) # directoryt's not required

		if(opts[:perf_stats_file]&.size > 0)
			perf_stats_vals = get_perf_stats_vals(opts[:perf_stats_file])
		else
			perf_stats_vals = []
		end

		json_stats_glob_path = opts[:json_stats_dir] + '/' + "*.json"
		json_stats_files = Dir.glob(json_stats_glob_path)

		# special hack for AMR runs
		if(json_stats_files.size == 0) 
			$stderr.puts "HACK HACK HACK special hack for amr omp runs -- remove probably"
			# see if they are in "runs/temp_latest"
			dir = File.join(opts[:test_dir], "runs/temp_latest")
			json_stats_glob_path = dir + '/' + "*.json"
			json_stats_files = Dir.glob(json_stats_glob_path)

			if(json_stats_files.size== 0 )
				raise "didn't find any files in #{json_stats_glob_path}"
			end

			puts "running: cp -f #{json_stats_glob_path} #{opts[:json_stats_dir]}..."
			`cp -f #{json_stats_glob_path} #{opts[:json_stats_dir]}`

			# now try again...		
			json_stats_glob_path = opts[:json_stats_dir] + '/' + "*.json"
			json_stats_files = Dir.glob(json_stats_glob_path)
		end

		raise "Unable to find any json logfiles with glob path #{json_stats_glob_path}. (pwd: #{FileUtils.pwd}) Exiting..." unless json_stats_files.size > 0
		
		# allow for special "rank 0 only reporting" mode.
		expected_num_logs = parse_intlike_arg(opts[:rank_0_stats_only]) == 1 ? 1 : opts[:total_threads]
		# allow the number of json files to be MORE than the number of logs by 2x or 4x
		if(json_stats_files.size == expected_num_logs || json_stats_files.size == expected_num_logs * 2 || json_stats_files.size == expected_num_logs * 4)
			# we are good
		else
			raise "Expected #{expected_num_logs} log files in directory #{opts[:json_stats_dir]}, but instead found #{json_stats_files.size}. Note for AMPI cases when we use virtual processors, we allow 2x or 4x the number of logs."
		end

		json_stats_files.each do |json_filename|
			thread_specific_vals = get_thread_specific_vals(json_filename, opts)
			# put out one row per thread
			puts (common_vals + thread_specific_vals + perf_stats_vals).join("\t")
		end
	else
		# just one row (but without performance info and other good data)
		puts common_vals.join("\t")
	end

end


if __FILE__ == $0
	main(opts)
end