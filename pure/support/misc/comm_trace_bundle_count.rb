#!/usr/bin/env ruby

# Author: James Psota
# File:   comm_trace_bundle_count.rb 

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



# This script processes the communication traces emitted from Pure to produce another format which
# is both human readible and in a format that Pure can use to determine things like bundle sizes, etc.

# Usage: comm_trace_bundle_count.rb <run_directory>

# Output format: <thread_id>, <process_id>, <channel_endpoint_tag>, <bundle_size>

require 'colorize'
require 'fileutils'
require 'active_support/core_ext/object/try'
require 'csv'
require "awesome_print"


class Message

	attr_reader :send_thread, :recv_thread, :channel_endpoint_tag, :seq_num, :send_proc, :recv_proc

	def initialize(threads_per_proc_arg, send_thread_arg, recv_thread_arg, channel_endpoint_tag_arg, seq_num_arg)

		@threads_per_proc = threads_per_proc_arg
		@send_thread = send_thread_arg
		@recv_thread = recv_thread_arg
		@channel_endpoint_tag = channel_endpoint_tag_arg
		@seq_num = seq_num_arg

		@send_proc = Message.thread_to_proc(@send_thread, @threads_per_proc)
		@recv_proc = Message.thread_to_proc(@recv_thread, @threads_per_proc)
	end

	def self.thread_to_proc(tid, threads_per_proc)
		(tid / threads_per_proc).floor
	end

	def to_s
		s = []
		s << "channel_endpoint_tag: #{@channel_endpoint_tag}"
		s << "send_thread: #{@send_thread}"
		s << "recv_thread: #{@recv_thread}"
		s << "send_proc: #{@send_proc}"
		s << "recv_proc: #{@recv_proc}"
		s << "seq_num: #{@seq_num}"
		s.join("\n")
	end

end


module CommTraceBundleUtils

	def self.process_comm_traces(dir, threads_per_proc, nprocs)

		sender_msg_db = {}
		receiver_msg_db = {}

		# structure: db[:channel_endpoint_tag][:process_msg_key_name:string] = <array of thread -> thread strings>:array

		expected_header_row = "direction, from, to, channel_endpoint_id, seq_num, channel_type, mpi_called"

		Dir.glob(File.join(dir, "comm_trace_rank_*.csv")) do |fname|
			line_num = 0

			File.open(fname) do |f|
				f.each_line do |line|
					line.strip!
					if(line_num == 0)
						raise "[line #{line_num}] Unexpected header row: #{line} for file #{fname}" unless line == expected_header_row
					else
						parts = line.split(',')
						raise "[#{fname}:#{line_num}] Invalid log format (send thread) #{parts[1]}" unless parts[1].strip.match(/\d+/)
						raise "[#{fname}:#{line_num}] Invalid log format (recv thread) #{parts[2]}" unless parts[2].strip.match(/\d+/)

						direction = parts[0].strip

						send_thread = parts[1].strip.to_i
						recv_thread = parts[2].strip.to_i

						channel_endpoint_tag = parts[3].strip.to_i
						seq_num = parts[4].strip.to_i

						msg = Message.new(threads_per_proc, send_thread, recv_thread, channel_endpoint_tag, seq_num)

						if(direction == "send")
							msg_db = sender_msg_db
						elsif(direction == "recv")
							msg_db = receiver_msg_db
						else
							raise "invalid direction #{direction}"
						end

						# initialization of structure for msg_db
						msg_db[msg.channel_endpoint_tag] ||= {}
						process_msg_key_name = "p#{msg.send_proc} → p#{msg.recv_proc}"
						msg_db[msg.channel_endpoint_tag][process_msg_key_name] ||= []

						# TODO: there's some sort of bug here as the log tracing is not showing up right.
						msg_db[msg.channel_endpoint_tag][process_msg_key_name] << "t#{msg.send_thread} → t#{msg.recv_thread}"	

					end # else
					line_num = line_num + 1
				end # for each line in file
			end # File open
		end # for each file

		return [sender_msg_db, receiver_msg_db]

	end # end method process_comm_traces

end

def main
	if ($0 == __FILE__)

		if(ARGV.size < 1)
			#raise "ERROR: Usage: #{$0} <run_directory> <threads_per_proc> <num_procs>"
		end

		# experiment web report: /Users/jim/Documents/Research/projects/hybrid_programming/pure/experiments/00314_NAS_DT_S_SH/experiment.html

		default_dir = "/Users/jim/Documents/Research/projects/hybrid_programming/pure/test/nas_pure/DT/runs/2015-12-22.15:10:06.477"
		default_nprocs = 4
		default_threads_per_proc = 3

		run_dir = ARGV.try(:[], 0) || default_dir
		threads_per_proc = ARGV.try(:[], 1).try(:to_i) || default_threads_per_proc
		nprocs = ARGV.try(:[], 2).try(:to_i)   || default_nprocs
		outfile = File.join(run_dir, "comm_trace_bundle_counts.csv")

		puts "Processing run directory #{run_dir} with args #{threads_per_proc}, #{nprocs}..."
		CommTraceBundleUtils.process_comm_traces(run_dir, threads_per_proc, nprocs)
		puts "\nDone."

	end
end


main()
