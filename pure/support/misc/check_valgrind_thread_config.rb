#!/usr/bin/env ruby

# Author: James Psota
# File:   check_valgrind_thread_config.rb 

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



STRING_TO_MATCH = "#include \"pure/support/valgrind_thread_includes.h\""
REGEX_TO_MATCH = /^#include\s+\"pure\/support\/valgrind_thread_includes\.h\"/m

files_to_check = ARGV.reject{|f| f.match(/valgrind_thread_includes\.h/)}

#puts "Checking valgrind thread configuration for #{files_to_check.join(', ')}."

faulty_files = []
files_to_check.each do |f|
    if(File.exist?(f))
        File.open(f) do |fd|
            matches = fd.read.match ( REGEX_TO_MATCH )
            if(matches.nil?)
                faulty_files << f
            end
        end
    end
end

if(faulty_files.size == 0)
    $stderr.puts "N.B. Source files look fine, but make sure to recompile if thread configuration was recently added to a Pure source header file."        
else
    $stderr.puts "ERROR: The following files are missing the required line: #{STRING_TO_MATCH}"
    faulty_files.map{|f| puts "* #{f}"}
    exit(-1)
end

