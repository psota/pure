#!/usr/bin/env ruby

# Author: James Psota
# File:   slurm_memory_footprint.rb 

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



# prints memory footprint every second for job id parameter

require 'open3'

def usage
    "#{$0} <slurm_job_id>"
end

raise usage unless ARGV.size >= 1
job_id = ARGV[0]
outfile = File.open(File.join(ENV['CPL'], 'support', 'experiments', 'memory_footprints', "mem_footprint_job:#{job_id}"), "w")

# Set up control-C handler
Signal.trap("INT") { 
    puts "Memory tracing terminating upon control-C. See output in\n#{outfile.path}"
    outfile.close
    exit
}

#############################################
# seen_lines are stripped lines that are treat lines deliniated by \n as separate lines
seen_lines = []

while true

    cmd = "sacct -j #{job_id} -o maxrss,maxrssnode,maxrsstask,MaxPages,MaxPagesNode,MaxPagesTask,MaxVMSize,MaxVMSizeNode,MaxVMSizeTask"
    stdout, status = Open3.capture2(cmd)

    if status.success?

        stdout.split("\n").each do |line|
            new_line = line&.strip

            # only print lines that haven't been seen yet
            if ! seen_lines.include?(new_line)
                outfile.puts new_line
                outfile.flush
                puts new_line 
                seen_lines << new_line
            end

        end
    else
        raise "unable to run command #{cmd}"
    end

    sleep 1

end # infinite loop; intention is to close this via control-c


