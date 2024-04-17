#!/usr/bin/env ruby

# Author: James Psota
# File:   gen_threadmaps.rb 

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

require 'colorize'
require 'fileutils'
require 'awesome_print'

TASKS_PER_NODE = [1, 2].freeze

private def threadmap_dir
  # assuming running this from the test directory
  File.join(FileUtils.pwd, 'threadmaps', 'craypat_based')
end

private def gridfilename(gridfile)
  File.basename(gridfile)
end

private def is_custom?(mode)
  mode == 'custom'
end

private def is_rr?(mode)
  mode == 'round_robin'
end

private def gen_mpi_threadmap(gridfile, mode)
  if is_custom?(mode)
    dest = File.join(threadmap_dir, 'mpi', "MPICH_RANK_ORDER.#{gridfilename(gridfile)}")
    FileUtils.mkdir_p(File.dirname(dest))
    FileUtils.cp(gridfile, dest)
    puts "Done generating custom MPI rankfile: #{dest}".green
  end
end

private def gen_custom_pure_threadmap(gridfile, num_nodes, tasks_per_node)
  #  B_SH_p3.threadmap
  total_tasks = num_nodes * tasks_per_node
  outfile_n = File.join(threadmap_dir, 'pure', "#{gridfilename(gridfile)}_p#{total_tasks}")
  FileUtils.mkdir_p(File.dirname(outfile_n))

  curr_task = 0
  expected_lines = 0

  unless tasks_per_node == 1 || tasks_per_node == 2
    raise "Right now, only 1 or 2 tasks per node is supported (currently is #{tasks_per_node}"
  end

  File.open(outfile_n, 'w') do |threadmap_file|
    File.open(gridfile) do |gridfile|
      gridfile.each_line do |line|
        next if line.strip[0] == '#'

        # just go through each line, making the pure threadmap as we go
        this_proc_ranks = line.strip.split(',')
        expected_lines += this_proc_ranks.size

        if tasks_per_node == 1
          this_proc_ranks.each do |rank|
            threadmap_file.puts "#{rank}:#{curr_task}"
          end
          curr_task += 1
          #########################################################################
        else
          # proc numa nodes -- split evenly -- 2 tasks per node. This does work with odd numbers, though. 
          nn_counts = []
          #nn_counts[0] = this_proc_ranks.size / 2  # nn0
          #nn_counts[1] = this_proc_ranks.size - nn_counts[0]

          # new version - 2022
          nn_counts[0] = (this_proc_ranks.size.to_f / 2.0).ceil  # nn0
          nn_counts[1] = this_proc_ranks.size - nn_counts[0]

          puts "counts: ".green
          ap nn_counts

          unless nn_counts[0] + nn_counts[1] == this_proc_ranks.size
            raise 'expected correct total of ranks'
          end

          curr_low_index = 0
          nn_counts.each do |nn_count|
            lo = curr_low_index
            hi = curr_low_index + nn_count

            this_proc_ranks[lo...hi].map { |r| threadmap_file.puts "#{r}:#{curr_task}" }
            curr_low_index = nn_count
            curr_task += 1
          end
        end

        unless curr_task <= total_tasks
          raise "MPI task too large -- #{curr_task}"
        end
      end
    end
  end

  assert_lines(outfile_n, expected_lines)
  puts "Done generating Pure threadmap: #{outfile_n}".green
end

private def gen_round_robin_pure_threadmap(gridfile, num_nodes, tasks_per_node, total_threads, threads_per_mpi_rank)
  # emulating MPICH_RANK_REORDER_METHOD=0
  # For example, an 8-process job launched on 4 dual-core nodes would be placed as:

  #        NODE   RANK
  #          0    0&4
  #          1    1&5
  #          2    2&6
  #          3    3&7
  # new approach: first get the node mapping, and then the mpi_rank mapping depending on tasks_per_node

  pure_node = []
  (0...total_threads).each do |pure_rank|
    node = pure_rank % num_nodes
    pure_node << [pure_rank, node]
  end

  # now determine the mpi_rank for each of these
  pure_ranks_per_node = (total_threads.to_f / num_nodes.to_f).ceil
  #pure_ranks_per_node = tasks_per_node * threads_per_mpi_rank

  # unless total_threads % num_nodes == 0
  #   raise 'Must be an even number of pure ranks per node'
  # end

  pure_ranks_per_mpi_rank = pure_ranks_per_node / tasks_per_node

  unless pure_ranks_per_node % tasks_per_node == 0
    raise 'Must be an even number of pure ranks per mpi rank for round robin mode'
  end

  # if tasks_per_node == 2
  #   puts "pure_ranks_per_mpi_rank = #{pure_ranks_per_mpi_rank}".red
  # end

  # sort by node
  pure_node = pure_node.sort_by { |pr| [pr[1], pr[0]] }

  # collect the [pure_rank, mpi_rank] pairs
  pure_mpi = []

  pure_rank_cnt = 0
  pure_node.each do |pn|
    pure_rank = pn[0]
    node = pn[1]

    mpi_rank = if tasks_per_node == 1
                 node
               else
                 if pure_rank_cnt < pure_ranks_per_mpi_rank
                   # low node if numa node
                   node * tasks_per_node
                 else
                   node * tasks_per_node + 1
                            end
               end

    puts "pure_rank: #{pure_rank}  pure_rank_cnt = #{pure_rank_cnt}   pure_ranks_per_mpi_rank = #{pure_ranks_per_mpi_rank}".yellow

    pure_rank_cnt += 1
    pure_rank_cnt = 0 if pure_rank_cnt == pure_ranks_per_node

    pure_mpi << [pure_rank, mpi_rank]
  end

  # sort by increasing MPI rank -- PureProcess needs it to be this way (although I forget why, exactly)
  pure_mpi = pure_mpi.sort_by { |pr| [pr[1], pr[0]] }

  # ap pure_mpi

  # write them out in mpi_rank order
  total_tasks = num_nodes * tasks_per_node
  outfile_n = File.join(threadmap_dir, 'pure', "#{gridfilename(gridfile)}_p#{total_tasks}")
  FileUtils.mkdir_p(File.dirname(outfile_n))
  File.open(outfile_n, 'w') do |threadmap_file|
    pure_mpi.each do |pr|
      threadmap_file.puts "#{pr[0]}:#{pr[1]}"
    end
  end

  assert_lines(outfile_n, total_threads)
end

private def assert_lines(filename, expected_lines)
  actual_lines = `wc -l #{filename}`.to_i
  unless actual_lines == expected_lines
    raise "Expected #{expected_lines} lines in file #{filename}, but there are #{actual_lines}"
  end
end

private def gen_pure_threadmaps(gridfile, num_nodes, mode, total_threads)
  TASKS_PER_NODE.each do |tasks|
    if is_custom?(mode)
      gen_custom_pure_threadmap(gridfile, num_nodes, tasks)
    elsif is_rr?(mode)
      gen_round_robin_pure_threadmap(gridfile, num_nodes, tasks, total_threads)
    end
  end
end

private def print_howto
  msg = %(
  REMINDER: To generate threadmap files, you must first generate the MPICH_RANK_ORDER.Grid file using CrayPat.

  1. Run make craypat-howto as a primer
  2. Configure BASELINE program in profile mode with representative options for a RELEASE run
  3. LOGIN WINDOW:  make clean && module load perftools-base perftools && make craypat-build
     INTERACTIVE WINDOW:  module load perftools-base perftools  && make craypat
  4. pat_report <output file>
  5. Verify that the MPICH_RANK_ORDER.Grid file is there (it may not be if it determines the default mapping is fine)
  6. Move it to the appropriate place (e.g., threadmaps/craypat_based/craypat_out with a better filename)
  7. Run this script
  8a. To run with MPI in custom mode, copy the MPI file to the test dir to MPICH_RANK_ORDER and then run with MPICH_RANK_REORDER_METHOD=3 MPICH_RANK_REORDER_DISPLAY=1
  8b. To run with MPI in round robin mode, run with MPICH_RANK_REORDER_METHOD=0 MPICH_RANK_REORDER_DISPLAY=1
  8c. To run with Pure, set THREAD_MAP_FILENAME to the appropriate threadmap file

  If this process doesn't generate a MPICH_RANK_ORDER.Grid file
)
  puts msg
end

private def main(gridfile, num_nodes, mode, total_threads)
  print_howto
  gen_mpi_threadmap(gridfile, mode)
  gen_pure_threadmaps(gridfile, num_nodes, mode, total_threads)
  puts "\nDone generating gridfiles for #{num_nodes} nodes (#{total_threads} total threads) from #{gridfile} in #{mode} mode\n".gray
end

##########################################
if $PROGRAM_NAME == __FILE__
  puts 'Reminder: call this from the top-level test dir with an argument such as: ' + 'threadmaps/craypat_based/craypat_out/B_SH'.bold
  puts 'WARNING: This only works for Cori right now.'.yellow.italic
  unless ARGV.size >= 3
    raise 'ERROR: Usage: gen_threadmaps.rb <craypat_output_grid_file> <num_nodes> <mode=custom or round_robin> <total_threads>'.red
  end

  gridfile = ARGV[0]
  num_nodes = ARGV[1].to_i
  mode = ARGV[2]&.strip
  unless mode == 'custom' || mode == 'round_robin'
    raise 'Invalid mode -- must be custom or round_robin'
  end

  total_threads = ARGV[3]&.to_i
  if is_rr?(mode)
    raise 'Invalid argument total threads' unless total_threads > 0
  end

  main(gridfile, num_nodes, mode, total_threads)
end
