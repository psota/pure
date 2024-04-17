#!/usr/bin/env ruby

# Author: James Psota
# File:   process_mpi_logs.rb 

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



test_dir = ARGV[0]
raise "Unable to find test directory #{test_dir}" unless Dir.exist?(test_dir)

mpi_log_dir = File.join(test_dir, 'mpi_debug_logs')
raise "Unable to find mpi log dir #{mpi_log_dir}" unless Dir.exist?(mpi_log_dir)

mpi_util_dir = ENV['MPICH_UTIL_DIR']
raise "Environment variable MPICH_UTIL_DIR must be set and a valid directory" unless mpi_util_dir && Dir.exist?(mpi_util_dir)

Dir.glob(File.join(mpi_log_dir, "dbg*.log")) do |log|
    puts log
    cmd = "perl #{mpi_util_dir}/dbg/getfuncstack.in < #{log} > #{log}.processed"
    puts cmd
    system(cmd)
end