#!/usr/bin/env ruby

# Author: James Psota
# File:   phys_core_id_to_verbose_name.rb 

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



NUMACTL_PHYSICAL_CORE_TO_VERBOSE_NAME_MAP = {
	0 => 'S0-C0-T0',
	1 => 'S0-C1-T0',
	2 => 'S0-C2-T0',
	3 => 'S0-C3-T0',
	4 => 'S0-C4-T0',
	5 => 'S0-C5-T0',
	6 => 'S0-C6-T0',
	7 => 'S0-C7-T0',
	8 => 'S1-C0-T0',
	9 => 'S1-C1-T0',
	10 => 'S1-C2-T0',
	11 => 'S1-C3-T0',
	12 => 'S1-C4-T0',
	13 => 'S1-C5-T0',
	14 => 'S1-C6-T0',
	15 => 'S1-C7-T0',
	16 => 'S0-C0-T1',
	17 => 'S0-C1-T1',
	18 => 'S0-C2-T1',
	19 => 'S0-C3-T1',
	20 => 'S0-C4-T1',
	21 => 'S0-C5-T1',
	22 => 'S0-C6-T1',
	23 => 'S0-C7-T1',
	24 => 'S1-C0-T1',
	25 => 'S1-C1-T1',
	26 => 'S1-C2-T1',
	27 => 'S1-C3-T1',
	28 => 'S1-C4-T1',
	29 => 'S1-C5-T1',
	30 => 'S1-C6-T1',
	31 => 'S1-C7-T1'
}.freeze


def strip_thread(verbose_name)
	verbose_name.gsub(/-T\d+$/, '')
end

def main(args)
	raise "ERROR: Usage: #{$0} <phys_core_ids (comma separated)>" unless args.size == 1 

	all_cores = args[0].split(/,/)
	final_output = []
	all_cores.each do |c|
		lookup = c&.to_i
		verbose_core = NUMACTL_PHYSICAL_CORE_TO_VERBOSE_NAME_MAP[lookup]
		raise "ERROR: invalid argument #{lookup}. Must be a value in #{NUMACTL_PHYSICAL_CORE_TO_VERBOSE_NAME_MAP.keys}" unless verbose_core
		final_output << strip_thread(verbose_core)
	end
	final_output.join(',')
end

if ($0 == __FILE__)
	if ARGV.size == 0
		# sometimes this method is chained and is called with no arguments. in that case, just return.
		exit
	else
		puts main(ARGV)
	end
end