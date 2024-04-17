#!/usr/bin/env ruby

# Author: James Psota
# File:   summarize_timers.rb 

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


require 'json'
require 'awesome_print'

raise "ERROR: Usage: #{$0} <logfile>" unless ARGV.size > 0

f = ARGV[0]
puts "processing file #{f}"

json = File.read(f)
obj = JSON.parse(json)
ap obj
elapsed = obj['elapsed_cycles']

timer_keys = obj.keys.grep(/.*_elapsed_cycles/)
final_timers = {}

timer_keys.each do |k|
    name_key = k.gsub(/_elapsed_cycles/, '') + '_timer_name'
    # puts name_key
    name = obj[name_key]
    quot = obj[k].to_f / elapsed.to_f * 100
#    puts "#{name}\t\t#{quot}%%"
    final_timers[name] = quot
end

final_timers = final_timers.sort_by{|k,v| -v}

##
ap final_timers
