#!/usr/bin/env ruby

# Author: James Psota
# File:   purify_all.rb 

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



require 'open3'
require 'colorize'

valid_extensions = %w{.c .cpp}

if (ARGV.size < 1)
	raise "You must pass in at least one file to purify_all.rb".colorize(:red)
end

ARGV.each do |f|

	next if !valid_extensions.any? { |ext| f.end_with?(ext) }

	cmd = "#{File.dirname(__FILE__)}/user_source_to_pure.rb -i #{f}"
	puts cmd
	cmd_out, status = Open3.capture2e(cmd)
	puts cmd_out

	if !status.success?
		raise "#{cmd} returned with error code #{status.to_s}"
	end

end


