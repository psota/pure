#!/usr/bin/env ruby

# Author: James Psota
# File:   asan_totals.rb 

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



# SUMMARY: AddressSanitizer: 68540 byte(s) leaked in 74 allocation(s).

leaked_bytes = []
leaked_allocations = []

ARGF.each_with_index do |line, idx|
	matcher = line.match /^SUMMARY: AddressSanitizer: (\d+) byte\(s\) leaked in (\d+) allocation\(s\)\.$/
	if(matcher)
		leaked_bytes << $1
		leaked_allocations << $2
	end
end

puts "Leaked Bytes (one line per #{leaked_bytes.size} processes):"
puts leaked_bytes
puts "sum:\n" + leaked_bytes.inject(0){|sum, elt| sum += elt.to_i}.to_s

puts "\nLeaked Allocations (one line per #{leaked_allocations.size} processes):"
puts leaked_allocations
puts "sum:\n" + leaked_allocations.inject(0){|sum, elt| sum += elt.to_i}.to_s
