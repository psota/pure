#!/usr/bin/env ruby

# Author: James Psota
# File:   determine_objs_dir.rb 

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
require 'colorize'
require 'fileutils'
require 'digest'
require 'awesome_print'

dir_parts = []

$opts = Optimist::options do
    opt :debug, "Debugging mode", :type=>:int, :default => 0
    opt :profile, "Profiling mode", :type=>:int, :default => 0
    opt :asan, "Enable Address Sanitizer", :type=>:int, :default => 0
    opt :tsan, "Enable Thread Sanitizer", :type=>:int, :default => 0
    opt :msan, "Enable Memory Sanitizer", :type=>:int, :default => 0
    opt :ubsan, "Enable Undefined Behavior Sanitizer", :type=>:int, :default => 0
    opt :trace_mpi_calls, "Enable MPI Tracer", :type=>:int, :default => 0
    opt :valgrind, "Enable valgrind", :type=>:int, :default => 0
    opt :thread_logger, "Enable thread logger", :type=>:int, :default => 0
    opt :trace_comm, "Enable tracing of message communication", :type=>:int, :default => 1
    opt :os, "Operating system", :type=>:string
    opt :pcv, "Process channel version", :type=>:int, :required=>true
    opt :print_pc_stats, "Print process channel stats", :type=>:int, :default=>0
    opt :cflags, "CFLAGS -- passed in directly from compiler options", :type=>:string
    opt :cxxflags, "CXXFLAGS -- passed in directly from compiler options", :type=>:string
    opt :lflags, "LFLAGS", :type=>:string
    opt :libs, "LIBS", :type=>:string
    opt :collect_timeline, "Collect timeline interval detail", :type=>:int, :default=>0

    opt :prepend_hostname_and_extra_prefix, "Add extra prefix. Useful for running many runs in parallel.", 
      :type=>:string, :default=>nil, :required=>false
    opt :verbose, "Verbosity", :type=>:bool, :default=>false
    opt :debugging_verbose, "Hardcore verbosity for hash debugging", :type=>:int, :default=>0

    opt :flags_hash_only, "Pass this if you only want back the flags hash (e.g., for the application settings)", :type=>:boolean, :default=>false
end

def assert_no_profile
	raise "Not able to have profiling enabled with other options\n#{$opts}".colorize(:red) if ($opts[:profile] == 1)
end
def assert_no_print_pc_stats
	raise "Not able to have printing of PC stats enabled with other options\n#{$opts}".colorize(:red) if ($opts[:print_pc_stats] == 1)
end
def assert_no_tsan
	raise "Not able to have TSAN enabled with other options\n#{$opts}".colorize(:red) if ($opts[:tsan] == 1)
end
def assert_no_msan
	raise "Not able to have MSAN enabled with other options\n#{$opts}".colorize(:red) if ($opts[:msan] == 1)
end
def assert_no_ubsan
	raise "Not able to have UBSAN enabled with other options\n#{$opts}".colorize(:red) if ($opts[:ubsan] == 1)
end
def assert_no_asan
	raise "Not able to have ASAN enabled with other options\n#{$opts}".colorize(:red) if ($opts[:asan] == 1)
end
def assert_no_valgrind
	raise "Not able to have Valgrind enabled with other options\n#{$opts}".colorize(:red) if ($opts[:valgrind] == 1)
end
def assert_no_thread_logging
	raise "Not able to have thread logging enabled other options\n#{$opts}".colorize(:red) if ($opts[:thread_logger] == 1)
end
def assert_no_trace_comm
	raise "Not able to have tracing of communication enabled other options\n#{$opts}".colorize(:red) if ($opts[:trace_comm] == 1)
end
def assert_osx
	raise "Must be running on OSX for these options\n#{$opts}".colorize(:red) if ($opts[:os] != 'osx')
end
def assert_cag
	raise "Must be running on cag for these options\n#{$opts}".colorize(:red) if ($opts[:os] != 'cag')
end
def assert_linux
	raise "Must be running on Linux for these options\n#{$opts}".colorize(:red) if (!($opts[:os] == 'cag' || $opts[:os] == 'nersc'))
end
def assert_no_mpi_tracing
	raise "Not able to have MPI tracing enabled with other options\n#{$opts}".colorize(:red) if ($opts[:trace_mpi_calls] == 1)
end
def assert_no_collect_timeline
	raise "Not able to collect thread timeline with other options\n#{$opts}".colorize(:red) if ($opts[:collect_timeline] == 1)
end

def prepare_flags(flags)
	flags.split(/\s+/).uniq.sort.join
end

if ($opts[:debug] == 1)

	# example directory would be like debug/ASAN/no_thread_logging
	dir_parts << 'debug'
	assert_no_profile

	if ($opts[:asan] == 1)
		assert_no_tsan
		assert_no_msan
		assert_no_ubsan
		assert_no_valgrind
		dir_parts << 'ASAN'
	elsif ($opts[:tsan] == 1)
		assert_no_asan
		assert_no_msan
		assert_no_ubsan
		assert_no_valgrind
		# testing this on OSX
		# assert_linux
		dir_parts << 'TSAN'
	elsif ($opts[:msan] == 1)
		assert_no_asan
		assert_no_tsan
		assert_no_ubsan
		assert_no_valgrind
		# testing this on OSX
		#assert_linux
		dir_parts << 'MSAN'
	elsif ($opts[:ubsan] == 1)
		assert_no_asan
		assert_no_tsan
		assert_no_msan
		assert_no_valgrind
#		assert_linux
		dir_parts << 'UBSAN'
	elsif ($opts[:valgrind] == 1)
		assert_no_asan
		assert_no_tsan
		assert_no_msan
		assert_no_ubsan
		dir_parts << "valgrind"
	else
		dir_parts << "no_checker"
	end

	if ($opts[:thread_logger] == 1)
		dir_parts << 'thread_logging'
	else
		dir_parts << 'no_thread_logging'
	end

	if ($opts[:trace_comm] == 1)
		dir_parts << 'trace_comm'
	else
		dir_parts << 'no_trace_comm'
	end

else 
	assert_no_msan
	assert_no_ubsan
	# assert_no_valgrind
	assert_no_thread_logging
	assert_no_trace_comm
	assert_no_mpi_tracing

	if ($opts[:profile] == 1)
		# profile
		dir_parts << "profile"
	else
		# release
		assert_no_profile
		assert_no_collect_timeline
		assert_no_print_pc_stats
		dir_parts << "release"
	end

	# assert_no_asan
	
	# testing ASAN and TSAN with profile mode
	#assert_no_tsan
	if ($opts[:tsan] == 1)
		$stderr.puts "WARNING: running TSAN in profile or release mode".colorize(:red)
		dir_parts << 'TSAN'
	end

	if ($opts[:asan] == 1)
		$stderr.puts "WARNING: running ASAN in profile or release mode".colorize(:red)
		dir_parts << 'ASAN'
	end
end

dir_parts << "pcv-#{$opts[:pcv]}" if $opts[:pcv]

if ($opts[:print_pc_stats] == 1)
	dir_parts << "print_pc_stats"
else
	dir_parts << "no_print_pc_stats"
end

# now, add in catch-all SHA1 of all flags
unique_flags = prepare_flags($opts[:cflags]) + ' ' + prepare_flags($opts[:cxxflags]) + ' ' + prepare_flags($opts[:lflags]) + ' ' + prepare_flags($opts[:libs])
flags_hash = Digest::SHA1.hexdigest unique_flags
dir_parts << flags_hash

if $opts[:debugging_verbose] == 1
	$stderr.puts $opts.awesome_inspect
	$stderr.puts "flags from parts #{unique_flags}".red + "\n --> #{flags_hash}".green
end

# possibly exit early here
if $opts[:flags_hash_only]
	puts flags_hash
	exit
end

# add optional extra prefix
if($opts[:prepend_hostname_and_extra_prefix]&.size &.> 0)
	# put these onto the front of the dir parts (in reverse order of desired)
	dir_parts.unshift($opts[:prepend_hostname_and_extra_prefix])
end

dir = dir_parts.join('/')
full_dir = File.join( File.dirname(__FILE__), '../../build', dir )

FileUtils.mkdir_p full_dir
build_opts_file = File.join(full_dir, "build_options.txt")
File.open(build_opts_file, 'w' ) do |f|
	f.puts $opts.inspect
end
if (ENV["PRINT_NODE_CONFIG"] || 0) == 1
	$stderr.puts "WROTE BUILD OPTIONS TO #{build_opts_file}".cyan
end

puts dir
