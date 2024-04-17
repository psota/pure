#!/usr/bin/env ruby

# Author: James Psota
# File:   valkyrie_select_input.rb 

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



# this program provides a simple interface to select a particular Valgrind XML file
# and open Valkyrie with it. It must be run from a test directory, which includes the
# XML files in a directory called "valgrind".

require 'colorize'

def main
	if ($0 == __FILE__)

		raise "ERROR: Usage: #{$0} <num_procs>" unless ARGV.size >= 1
		num_procs = ARGV[0].to_i

		# check invariants
		valgrind_dir = 'valgrind'
		raise "ERROR: directory #{valgrind_dir} not found." unless Dir.exist?(valgrind_dir)

		cmd = "ls -t #{valgrind_dir}/memcheck-*.valgrind.xml | head -#{num_procs}"
		log_files = `#{cmd}`.split(/\n/)

		puts "\nWhich Valgrind log file would you like to view with Valkyrie?"
		log_files.each_with_index do |l, i|
			wc_output = `wc -l #{l}`
			matcher = wc_output.match(/\s*(\d+)\s*.*/)
			num_lines = matcher[1].to_i
			formatted_line = "#{i+1}: #{l} (#{matcher[1]} lines)"
			
			no_error_line_count = 55 # based on one example
			if(num_lines > no_error_line_count)
				puts "  " + formatted_line
			else
				puts "✓ ".colorize(:green) + formatted_line.colorize(:light_black)
			end
		end

		user_input = 0

		while(1)
			print "\nWhich file? > "

			user_input = $stdin.gets

			if(user_input.strip.match(/\s*q.*/i)) 
				exit
			end

			user_input = user_input.to_i

			if(user_input <= 0 || user_input > num_procs)
				$stderr.puts "Invalid entry; try again."
			else
				break
			end
		end

		selected = log_files[user_input-1]
		puts "Running valkyrie on #{selected} (note: the window may be initially hidden)."
		cmd = "valkyrie --view-log #{selected} &"
		system(cmd)
	end
end


main()