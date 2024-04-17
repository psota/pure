#!/usr/bin/env ruby

# Author: James Psota
# File:   label_ranks_on_core_topo.rb 

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

require 'csv'
require 'awesome_print'
require 'colorize'
require 'open3'
require 'fileutils'

def x_offset(core)
  left_margin = 30

  column = core.to_i % 16
  core_width = 92

  core_margin = 10

  left_margin + column * core_width + core_margin
end

def y_offset(core)
  # just a simple category approach
  top = case core.to_i
        when 0..15
          300
        when 32..47
          330
        when 16..31
          670
        when 48..63
          700
        else
          raise "Invalid core id #{core} for CPU_MODEL #{ENV['CPU_MODEL']}"
  end

  top + 10
end

# creates the attribute text for a single core
def label_core_attr(label, core_num)
  x = x_offset(core_num) + 2
  y = y_offset(core_num) + 1
  "-pointsize 8 -draw \"text #{x},#{y} '#{label}'\""
end

def crop_footer(outfile)
  # crop out bottom
  cmd = "convert #{outfile} -gravity South -chop 0x70 #{outfile}"
  stdout, status = Open3.capture2(cmd)
  raise "unable to run command #{cmd}" unless status.success?
end

COLORS = ['003049', 'D62828', 'F77F00', '3A506B'].freeze
def color_for_mpi(rank)
  COLORS[rank % COLORS.size]
end



def main
  raise 'ERROR: Usage: $0 <core_map>' if ARGV.size < 1
  raise "Only cori supported" unless ENV['CPU_MODEL'] == "Intel(R) Xeon(R) CPU E5-2698 v3 @ 2.30GHz" || ENV['OS'] == 'osx'

  dir = "topology"
  FileUtils.mkdir_p(dir)
  file_prefix = "core_topo.#{ENV['NUMA_SCHEME']}"
  FileUtils.rm_rf(File.join(dir, file_prefix + '*.pdf'))
  infile = File.join(dir, "#{file_prefix}.png")

  infile_orig = File.join(ENV['CPL'], 'support', 'topologies', 'cori_haswell_compute_node.png')
  FileUtils.cp(infile_orig, infile)

  # cori login node was different so using a cached copy of topology from a compute node.
  # FileUtils.rm_f infile
  # generate the topo img
  # cmd = "lstopo --no-io #{infile}"
  # stdout, status = Open3.capture2(cmd)
  # raise "unable to run command #{cmd}" unless status.success?

  # use pre-generated topology file and make a copy

  core_map = ARGV[0]
  table = CSV.parse(File.read(core_map), headers: true)

  hostnames = table.map { |r| r['hostname'] }.uniq.map(&:strip)
  hostnames.each do |hostname|
    #puts "PROCESSING NODE #{hostname}".yellow
    # parse input file and build up the command to label the cores
    cmd = []
    cmd << "convert #{infile}"
    cmd << '-fill "#EF4136" -font Helvetica-Bold'

    table.each do |row|
      hn = row['hostname']
      next unless hn.strip == hostname

      lab = "r#{row['pure_thread_rank'].to_i} #{row['pure_thread_name']}"

      matcher = row['pure_thread_name'].match(/p\d\d\d\dm(\d\d\d\d)t\d\d\d\d/)
      mpi_rank = matcher[1].to_i
      color = color_for_mpi(mpi_rank)
      cmd << "-fill \"##{color}\""

      core_num = row['cpu_number']
      cmd << label_core_attr(lab, core_num)
    end

    # also, label the node id
    cmd << "-fill \"#51489D\" -pointsize 24 -draw \"text #{1522 / 2 - 50},25 '#{hostname}'\""

    # TODO: consider using PDF
    outfile = infile.gsub('.png', '.') + hostname + '.pdf'
    puts "Labeling #{infile} and creating #{outfile}".green

    # output file
    cmd << outfile

    #########################################
    full_cmd = cmd.join(' ')
    # puts full_cmd.red
    stdout, status = Open3.capture2(full_cmd)
    raise "unable to run command #{full_cmd}" unless status.success?

    crop_footer(outfile)
  end
end

main
