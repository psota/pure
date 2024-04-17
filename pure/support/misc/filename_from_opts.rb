#!/usr/bin/env ruby

# Author: James Psota
# File:   filename_from_opts.rb 

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



require 'fileutils'

# come up with good filename based on all passed options

sep = (ENV['SEPERATOR'] || '_').freeze
ext = (ENV['EXTENSION'] || '').freeze

curr_dir = FileUtils.pwd
test_dir = File.join(ENV['CPL'], 'test/')
test_subdir = curr_dir.gsub(test_dir, '').gsub('/', '-')

name_parts = []
name_parts << test_subdir

if(ENV['RUN_ARGS'].size < 16)
    # run args can get really long, so only include if short
    name_parts << ENV['RUN_ARGS']
end
name_parts << ENV['NUMA_SCHEME']
name_parts << (ENV['PROCESS_CHANNEL_VERSION'] ? "pcv#{ENV['PROCESS_CHANNEL_VERSION']}" : nil)
name_parts << (ENV['PROCESS_CHANNEL_BUFFERED_MSG_SIZE'] ? "buf#{ENV['PROCESS_CHANNEL_BUFFERED_MSG_SIZE']}" : nil)
name_parts << ENV['FLAGS_HASH']

prelim_sep = ' '.freeze
# deal with spaces in env vars
# you can pass addition args here
name_parts << ARGV.join(prelim_sep)

basename = name_parts.compact.map(&:strip).join(prelim_sep).squeeze(prelim_sep).strip.gsub(prelim_sep, sep)

basename = ENV['RUNTIME'] + ':' + basename

# return value passed to STDOUT
fname = basename + ext
$stderr.puts "Flamegraph filename: #{fname}"
$stdout.puts fname