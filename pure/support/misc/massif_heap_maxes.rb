#!/usr/bin/env ruby

# Author: James Psota
# File:   massif_heap_maxes.rb 

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



raise "ERROR: Usage: #{$0} <massif_output_files>" unless ARGV.size > 0

#  Logs look like this:
#
#  #-----------
#  snapshot=23
#  #-----------
#  time=13408220
#  mem_heap_B=4256744
#  mem_heap_extra_B=5184
#  mem_stacks_B=0
#  heap_tree=peak

# Output format: heap footprint total (B), 4822829

total = 0
$DEBUG = false

ARGV.each do |massif_out|
	raise "Unable to find massif output file #{massif_out}" unless File.exist?(massif_out)

	heap_amounts = []

	File.open(massif_out) do |f|
		matching_lines = f.grep(/mem_heap_B=\d+/)

		matching_lines.each do |l|
			matcher = l.match /mem_heap_B=(\d+)/
			heap_amounts << $1.to_i
		end
	end

	max_heap = heap_amounts.max
	total += max_heap
	puts "#{massif_out}, #{heap_amounts.max}" if $DEBUG
end

#puts "heap footprint total (B), #{total}"
puts "#{total}"