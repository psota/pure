#!/usr/bin/env ruby

# Author: James Psota
# File:   ms_print_totals.rb 

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



raise "ERROR: Usage: #{$0} <ms_print_output_files>" unless ARGV.size > 0

ARGV.each do |ms_print_out|
	raise "Unable to find ms_print output file #{ms_print_out}" unless File.exist?(ms_print_out)
	cmd = "sed '9q;d' #{ms_print_out} | egrep -Eo '[0-9.]+'"
	success = system(cmd)
	raise "Command #{cmd} failed" unless success
end