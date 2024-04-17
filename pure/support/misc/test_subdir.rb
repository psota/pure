#!/usr/bin/env ruby

# Author: James Psota
# File:   test_subdir.rb 

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



require 'colorize'

# example:
# given a present working directory that is inside a test directory, return the subdirectory
# under the test directory. For example, "/Users/jim/Documents/Research/projects/hybrid_programming/pure/test/ping_pong/pure" as 
# the input will return "ping_pong/pure"

raise "ERROR: Usage: #{$0} <test_dir>" unless ARGV.size >= 1

REPO_TEST_PARENT_DIR = File.join(ENV['CPL'], 'test').freeze

def main
    td = ARGV[0]

    test_dir_matcher = td.match /(.*)#{REPO_TEST_PARENT_DIR}(.*)/
    playground_matcher = td.match /(.*)playground(.*)/

    if test_dir_matcher && test_dir_matcher[2]
        #$stderr.puts "TEST SUBDIR: #{test_dir_matcher[2]}".cyan
        puts test_dir_matcher[2]
    elsif playground_matcher && playground_matcher[2]
        puts playground_matcher[2]
    else
        abort "Invalid argument '#{td}': path must contain #{REPO_TEST_PARENT_DIR}".red
        exit 2
    end    
end

main