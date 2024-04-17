#!/usr/bin/env ruby

# Author: James Psota
# File:   clang_tidy_many.rb 

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



# runs clang-tidy on all files passed in on command line using relative or absolute paths
# puts output on a file-by-file basis in .tidy directory in same directory as source file

require 'colorize'
require 'fileutils'

$DEBUG = true
$INCLUDE_DIR = File.expand_path(File.join( File.dirname(__FILE__), '../..', 'include' )).freeze
$TIDY_DIR_NAME = '.tidy'.freeze

def clang_tidy(file_path, clean_output=true, do_fix=false)

	header_filter = File.join( $INCLUDE_DIR, 'pure' )
	outfile_dir = File.join( File.dirname(file_path), $TIDY_DIR_NAME )
	
	if( ! File.exists?(outfile_dir) )
		FileUtils.mkdir(outfile_dir)
	end

	outfile = File.join( outfile_dir, File.basename(file_path) + '.tidy' )
	if do_fix
		outfile += '.fixed'
	end

	extra_flags = []
	extra_flags << "-DLINUX" if(ENV['OS'] == 'cag') 
	extra_flags << "-DOSX" if(ENV['OS'] == 'osx') 
	extra_flags << "-DTRACE_COMM=1"
	extra_flags << "-DDEBUG_CHECK"
	extra_flags << "-DENABLE_THREAD_LOGGER"

	extra_flags = extra_flags.join(' ')

	cmd = "clang-tidy -header-filter=#{header_filter} #{do_fix ? '-fix' : ''} #{file_path} -- -Wall -x=c++ -std=c++11 -Drestrict= #{extra_flags} -fdiagnostics-color=always -g -fno-inline -O0 -I#{$INCLUDE_DIR} -I#{$INCLUDE_DIR}/pure -I#{$INCLUDE_DIR}/pure/common -I/Users/jim/local/include/ -I/Users/jim/local/src/boost_1_59_0 > #{outfile}"
	puts cmd
	system(cmd)

	if(clean_output)
		# read file into string and keep calling cleaner passes
		File.open(outfile) do |f|
			file_str = f.read

			# cleaning passes
			file_str = clean_old_asserts(file_str)
			file_str = clean_missing_header_guard(file_str)
			file_str = clean_pointer_arithmetic(file_str)
			file_str = clean_using_in_global_namespace(file_str)
			file_str = clean_c_style_vararg(file_str)

			File.open(outfile + '.cleaned', 'w') do |clean_f|
				clean_f.puts(file_str)
			end
		end
	end

end


def skip_matching_warnings(regex_for_head_match, log_string)

	output_str = []
	log_parts = log_string.split("\n")

	# hack: we assume that the clang warnings always start with the absolute path of the directory where this code is checked out
	dir_prefix = '/' + $INCLUDE_DIR.split(/\//)[1..3].join('/')
	start_of_warning_regex = /^#{dir_prefix}.*:\d+:\d+: (?:warning|error):.*/
	puts start_of_warning_regex if $DEBUG

	l = 0

	while l < log_parts.size do 
		line = log_parts[l]
		if(line.match(regex_for_head_match))
			# if this is true, this is the first line of something to skip.
			# skip to the beginning of the next error
			puts "Skipping: #{log_parts[l]}".colorize(:yellow) if $DEBUG
			l = l + 1 # skip the header line

			while( ! log_parts[l].match(start_of_warning_regex) )
				puts "Skipping: #{log_parts[l]}".colorize(:yellow) if $DEBUG
				l = l + 1
				if( l >= log_parts.size) 
					break
				end
			end
		else
			output_str << line
			puts "Keeping: #{log_parts[l]}".colorize(:light_black) if $DEBUG
			l = l + 1
		end
	end

	output_str.join("\n")
end

def clean_old_asserts(log_string)

=begin
/Users/jim/Documents/Research/projects/hybrid_programming/pure/src/runtime/pure.cpp:44:2: warning: do not implicitly decay an array into a pointer; consider using gsl::array_view or an explicit cast instead [cppcoreguidelines-pro-bounds-array-to-pointer-decay]
        assert(init_provided_thread_support == MPI_THREAD_MULTIPLE);
        ^
/usr/include/assert.h:93:47: note: expanded from macro 'assert'
    (__builtin_expect(!(e), 0) ? __assert_rtn(__func__, __FILE__, __LINE__, #e) : (void)0)
                                              ^
=end

	re = /.*warning: do not implicitly decay an array into a pointer; consider using gsl::array_view or an explicit cast instead \[cppcoreguidelines-pro-bounds-array-to-pointer-decay\]/
	skip_matching_warnings(re, log_string)
end

def clean_missing_header_guard(log_string)

=begin
/Users/jim/Documents/Research/projects/hybrid_programming/pure/src/runtime/../../support/misc/../../include/pure/common/pure_debug.h:1:1: warning: header is missing header guard [llvm-header-guard]
#pragma once
^
=end

	re = /.*warning: header is missing header guard \[llvm-header-guard\]/
	skip_matching_warnings(re, log_string)
end

def clean_pointer_arithmetic(log_string)

=begin
/Users/jim/Documents/Research/projects/hybrid_programming/pure/include/pure/runtime/pure_process.h:110:12: warning: do not use pointer arithmetic [cppcoreguidelines-pro-bounds-pointer-arithmetic]
    return pure_rank_to_mpi_rank[pure_thread_rank];
           ^
=end
	re = /.*warning: do not use pointer arithmetic \[cppcoreguidelines-pro-bounds-pointer-arithmetic\]/
	skip_matching_warnings(re, log_string)
end

def clean_using_in_global_namespace(log_string)

=begin
/Users/jim/Documents/Research/projects/hybrid_programming/pure/include/pure/support/thread_logger.h:24:1: warning: using declarations in the global namespace in headers are prohibited [google-global-names-in-headers]
using std::unordered_map;
^
=end

	re = /warning: using declarations in the global namespace in headers are prohibited \[google-global-names-in-headers\]/
	skip_matching_warnings(re, log_string)
end

def clean_c_style_vararg(log_string)
	re = /warning: do not call c-style vararg functions \[cppcoreguidelines-pro-type-vararg\]/
	skip_matching_warnings(re, log_string)
end

def main
	raise "ERROR: Usage: #{$0} [-fix] <file_to_process_1> ... <file_to_process_n>".colorize(:red) unless ARGV.size >= 1

	# parse out -fix flag
	fix_regex = /-?-fix/
	if(ARGV.join(' ').match(fix_regex))
		cleaned_argv = ARGV.reject{|a| a.match(fix_regex)}
		do_fix = true
		print "Running clang-tidy with " + "fixes enabled".colorize(:green) + " on #{cleaned_argv.size} file"
	else
		cleaned_argv = ARGV
		do_fix = false
		print "Running clang-tidy with " + "fixes disabled".colorize(:red) + " on #{cleaned_argv.size} file"
	end
	if (cleaned_argv.size != 1)
		puts "s"
	else
		puts
	end

	cleaned_argv.each do |file_path|
		raise "ERROR: unable to find file #{file_path}" unless File.exists?(file_path)
		puts "\nclang-tidying #{file_path}".colorize(:light_black)
		clean_output = true
		clang_tidy(file_path, clean_output, do_fix)
	end
end


if ($0 == __FILE__)
	main()
end