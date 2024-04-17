#!/usr/bin/env ruby

# Author: James Psota
# File:   longest_elapsed_time_from_json.rb 

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


# frozen_string_literal: true

require 'fileutils'
require 'colorize'
require 'open3'
require 'awesome_print'
require 'fileutils'
require 'descriptive_statistics'
require 'active_support'
require 'active_support/core_ext/numeric/conversions'

unless ARGV.size >= 2
  raise "ERROR: Usage: #{$PROGRAM_NAME} <num_threads> <json_dir> <timer=elapsed_cycles>".red
end

def cycles_to_time(cycles)
  if ENV['OS'] == 'osx'
    ''
  else
    cmd = "lscpu | grep 'CPU max MHz'"
    stdout, status = Open3.capture2(cmd)

    if status.success?
      m = stdout.match(/CPU max MHz:\s+(\d+\.?\d+)/)
      raise 'Unable to find CPU frequency' unless m

      mhz = m[1].to_f
    else
      # try again with a less precise listing
      cmd = "lscpu | grep 'CPU MHz'"
      stdout, status = Open3.capture2(cmd)
      raise "Unable to run the command: #{cmd}" unless status.success?

      m = stdout.match(/CPU MHz:\s+(\d+\.?\d+)/)
      raise 'Unable to find CPU frequency' unless m

      mhz = m[1].to_f
    end

    seconds = cycles / (1_000_000.0 * mhz.to_f)
    formatted = Time.at(seconds).utc.strftime('%H:%M:%S:%L')
    formatted
  end
end

def get_field(field, file)
  cmd = "jq \"#{field}\" #{file}"
  # $stdout.puts cmd.green
  stdout, status = Open3.capture2(cmd)
  raise "unable to run command #{cmd}" unless status.success?

  stdout.strip.gsub('"', '')
end

def shell_wait
  cmd = 'wait'
  stdout, status = Open3.capture2(cmd)
  raise "unable to run command #{cmd}" unless status.success?
end

def timer_details(threads, json_dir_arg, timer_name)
  num_threads = threads.to_i
  unless num_threads > 0
    raise "num_threads (#{num_threads}) must be greater than zero"
  end

  json_dir = json_dir_arg
  files = Dir.glob("#{json_dir}/stats*.json").sort_by { |f| File.mtime(f) }
  files.reverse!

  focus_timer = timer_name
  focus_timer_json = ".#{focus_timer}"

  # pull out custom name
  if focus_timer.match /^custom/
    focus_timer_name_field = focus_timer.gsub(/elapsed_cycles/, 'timer_name')
    focus_timer_name = get_field(".#{focus_timer_name_field}", files[0])
  else
    focus_timer_name = focus_timer
  end

  # also create a node map file, which we drop in the json dir
  rankfile = File.join(json_dir, 'rank_to_cpu_map.csv')

  times = []
  ranks = []
  File.open(rankfile, 'w') do |node_file|
    files[0...num_threads].each do |f|
      line = get_field("[ .pure_thread_rank, .cpu_number, .hostname, .pure_thread_name, #{focus_timer_json}]", f)
      line = line.gsub(/[\n\[\]]/, '')

      parts = line.split(',')
      rank = parts[0]
      cpu_number = parts[1]
      node = parts[2]
      thread_name = parts[3]
      cycles = parts[4].strip.to_i
      times << cycles
      ranks << rank
      node_file.printf("%.4d, %s, %d, %s\n", rank, thread_name, cpu_number, node)
    end
  end

  cycles = times.max
  rank_of_max = ranks[times.index(times.max)].strip.to_i

  cmd = "sort -n #{rankfile} -o #{rankfile}"
  stdout, status = Open3.capture2(cmd)
  raise "unable to run command #{cmd}" unless status.success?

  # now add the header after it's been sorted
  tempfile = '___tempfile.283293823'
  out = File.open(tempfile, 'w')
  File.open(rankfile, 'r') do |node_file|
    out.puts('pure_thread_rank,pure_thread_name,cpu_number,hostname')
    node_file.each_line do |l|
      out.puts l
    end
  end
  out.close

  FileUtils.mv(tempfile, rankfile)
  FileUtils.rm_f(tempfile)

  mean = times.reject { |e| e == 0 }.mean.round
  { name: focus_timer_name, cycles: cycles, max_rank: rank_of_max, mean: mean }
end

################################

def report
  default_timer_name = 'elapsed_cycles'
  timer = ARGV[2] || default_timer_name

  num_threads = ARGV[0].to_i
  json_dir = ARGV[1]
  details = timer_details(num_threads, json_dir, timer)

  if timer == default_timer_name
    # just no percentage
    print "\n#{details[:name]}:\t max: #{delim(details[:cycles])} (rank #{details[:max_rank]})\t\tmean: #{delim(details[:mean])}"
  else
    # compute percentage of time for the rank that has the max

    # figure out filename
    run_files = Dir.glob("#{json_dir}/stats*.json").sort_by { |f| File.mtime(f) }.reverse[0...num_threads]
    found_files = run_files.grep /.*rank_#{details[:max_rank]}\.json.*/
    raise "unable to find file for rank #{details[:max_rank]}" if found_files.size != 1

    total_cycles_for_rank = get_field('.elapsed_cycles', found_files[0])
    perc = ((details[:cycles].to_f / total_cycles_for_rank.to_f) * 100).round(2)
    raise "Percentage was #{perc}! for timer #{details[:name]}, rank #{details[:max_rank]}" if perc > 100

    if perc < 5
      print "#{perc}% "
    elsif perc < 25
      print "#{perc}% ".green
    elsif perc < 50
      print "#{perc}% ".yellow
    else
      print "#{perc}% ".red
    end

    print " -- " + "#{details[:name]}".bold + ", "
    print "rank #{delim(details[:max_rank])} (rank with most time in #{details[:name]})"
    print "\tmax (rank #{details[:max_rank]}): #{delim(details[:cycles])} / #{delim(total_cycles_for_rank)} \t\tmean of all ranks: #{delim(details[:mean])}"
  end

  # special consideration for running this script on a login node (doesn't have the same CPU freq)
  if !ENV['SLURM_STEP_NUM_TASKS'].nil?
    formatted_time = cycles_to_time(details[:cycles])
    print "\t#{formatted_time} max time"
  end
  puts
end

private def delim(n)
  ActiveSupport::NumberHelper.number_to_delimited(n)
end

report if $PROGRAM_NAME == __FILE__
