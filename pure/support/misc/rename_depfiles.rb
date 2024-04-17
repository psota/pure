#!/usr/bin/env ruby

# Author: James Psota
# File:   rename_depfiles.rb 

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



#require 'awesome_print'
require 'open3'
require 'colorize'

raise "Usage: #{$0} <depdir>" unless ARGV.size >= 1

depdir = ARGV[0]

cmd = "find #{depdir} -name *.Td"
#puts cmd
stdout, status = Open3.capture2(cmd)
raise "unable to run command #{cmd}" unless status.success?

files = stdout.split /\s/
files.each do |f|
    matcher = f.match(/^(.*)\.Td$/)
    cmd = "mv -f #{matcher[1]}.Td #{matcher[1]}.d"
    puts cmd.yellow
    stdout, status = Open3.capture2(cmd)
    raise "unable to run command #{cmd}" unless status.success?
end
