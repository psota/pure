#!/usr/bin/env ruby

# Author: James Psota
# File:   profile_running_job.rb 

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
require 'awesome_print'
require 'fileutils'
require 'colorize'
require 'simple_stats'

PERCENT_THRESHOLD = 0.3.freeze

private def profile_pid(pid, perf_events, timestamp)
    d = "profiles/#{timestamp}"
    FileUtils.mkdir_p(d)
    cmd = "ocperf.py record -q -e #{perf_events} -p #{pid} -o #{File.join(d, 'perf.data.' + pid)}"
    puts cmd
    return Process.spawn(cmd)
end

private def shell_cmd(cmd, error_msg=nil)
    stdout, status = Open3.capture2(cmd)

    raise (error_msg || "unable to run command #{cmd}") unless status.success?
    return stdout
end

private def load_symbols!(report, symbol_hash, shared_obj_stats)

    File.readlines(report).each do |l|
        #puts "processing line #{l} of file #{report}"

        #  3.68%  ,CoMD-mpi@ff2437,libmpi.so.12.1.1                                 ,[.] MPIDI_CH3I_Progress
        m = l.match(/\s*(\d+\.\d+)%\s*,(.*?),(.*?),\[.*\](.*)/)
        if(m)
            p = m[1].to_f
            c = m[2].strip
            so = m[3].strip
            sym = m[4].strip

            symbol_hash[sym] ||= {}
            symbol_hash[sym][report] ||= 0.0
            symbol_hash[sym][report] += p

            shared_obj_stats[so] ||= {}
            shared_obj_stats[so][report] ||= 0.0
            shared_obj_stats[so][report] += p
        else
            puts "skipping line #{l}"
        end
    end
end

private def print_report(label, data, nprocs, file, report=:full)

    file.print "#{label}\tmin %\tmean %\tmax %"
    if(report == :full)
        print "\t"
        nprocs.times do |i|
            file.print "proc " + i.to_s + "\t"
        end
    end
    file.puts

    data.each_pair do |label, file_percent_h|
        percents = file_percent_h.values
        if(percents.sum >= PERCENT_THRESHOLD)
            vals = []
            vals << label
            vals << percents.min
            vals << percents.mean
            vals << percents.max
            vals += percents if (report == :full)

            file.puts vals.join("\t")
        end
    end
end

#############################################################
raise "ERROR: Usage: #{$0} <bin_name> <nprocs> <perf_events> <timestamp>" unless ARGV.size >= 4

# process names are truncated to 15 characters (pgrep)
bin = ARGV[0][0..14]
nprocs = ARGV[1].to_i
perf_events = ARGV[2]
ts = ARGV[3]

cmd = "pgrep -u jim PARENT_M_"
stdout = shell_cmd(cmd, "\n\nERROR: Benchmark not running; it must be actively running before this profile program is run\n\n".red)

pids = stdout.split(/\n/)
raise "Expected to find #{nprocs} pids but found #{pids.size} instead. Make sure to wait to run 'make profile-local-mpi' until the program is already running." unless pids.size == nprocs

perf_pids = []
pids.each do |p|
    perf_pids << profile_pid(p, perf_events, ts)
end    

# puts "perf pids"
# ap perf_pids

perf_pids.each do |pp|
    Process.wait(pp)
end

###############################################################
# now report on it.
perf_data = Dir.glob(File.join("profiles/#{ts}", "perf.data.*"))
#ap perf_data
raise unless perf_data.size == nprocs

perf_data.each do |pd|
    cmd = "ocperf.py report -q --stdio -t, -i #{pd} > #{pd}.txt"
    puts cmd
    stdout = shell_cmd(cmd)
    puts "perf output [#{pd}]:" + stdout
end

###############################################################
# now compute statistics
symbol_stats = Hash.new
shared_obj_stats = Hash.new

reports = Dir.glob(File.join("profiles/#{ts}", "perf.data.*.txt"))

puts "processing reports #{reports}"

reports.each do |r|
    load_symbols!(r, symbol_stats, shared_obj_stats)
end

###############################################################
# now generate report


File.open(File.join("profiles/#{ts}/report_full.txt"), 'w') do |f|
    f.puts "\nStatistics across #{nprocs} processes on this node (#{`hostname`.strip})\n"

    f.puts "\n=== SHARED OBJECT BREAKDOWN ===\n"
    print_report('shared object', shared_obj_stats, nprocs, f, :full)

    f.puts "\n=== SYMBOL BREAKDOWN ===\n"
    print_report('symbol', symbol_stats, nprocs, f, :full)
end

File.open(File.join("profiles/#{ts}/report.txt"), 'w') do |f|
    f.puts "\nStatistics across #{nprocs} processes on this node (#{`hostname`.strip})\n"

    f.puts "\n=== SHARED OBJECT BREAKDOWN ===\n"
    print_report('shared object', shared_obj_stats, nprocs, f, :simple)

    f.puts "\n=== SYMBOL BREAKDOWN ===\n"
    print_report('symbol', symbol_stats, nprocs, f, :simple)
end












puts "\nSee profile results in profiles/#{ts}/report.txt\n".green
