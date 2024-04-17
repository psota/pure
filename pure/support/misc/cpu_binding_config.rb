#!/usr/bin/env ruby

# Author: James Psota
# File:   cpu_binding_config.rb 

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



require 'awesome_print'
require 'colorize'

# get the numa ranges with: lscpu | tail -3
raise "ERROR: Usage: #{$0} <numa0 ranges> <numa1 ranges> <seq | seq_ht | alt | alt_ht>" unless ARGV.size > 3

private def array_from_arg(arg, pack_ht)
    # input looks like "0-11,24-35"
    out = []

    # two modes:
    # one mode has ranges of cpu ids, another has a comma-delimited list

    if(arg.match("-"))
        $stderr.puts "USING RANGE OF CPUS VERSION".gray
      # range version  
        parts = arg.split(',')
        parts.each do |r|
            rparts = r.split('-')
            raise "invalid range #{r}" unless rparts.size == 2

            new_elts = (rparts.first..rparts.last).to_a
            if (out.size == 0)
                out += new_elts
            else
                if(pack_ht)
                    # alternate in
                    out = out.zip(new_elts)
                else
                    out << new_elts
                end
            end
        end
    else
        # list version
        $stderr.puts "USING LIST OF CPUS VERSION".gray
        out = arg.split(',')
    end

    out.flatten
end

private def alternate_n(l1, l2, n)

    raise "Lists must be the same length" unless l1.size == l2.size

    out = []
    sz = l1.size
    i = sz
    while(i > 0) do
        take_amount = [n, i].min
        out += l1.shift(take_amount)
        out += l2.shift(take_amount)
        i   -= take_amount
    end

    raise "Expected #{2*sz} elements in output array, but got #{out.size}" unless out.size == (2*sz)
    out
end


mode = ARGV.last
pack_hyperthreads = !mode.match(/.*_ht/).nil?

pieces = []
(0..7).each do |piece|
    pieces += array_from_arg(ARGV[piece], pack_hyperthreads)
end

case mode
when /^seq/
    out = pieces.flatten

# only do seq for perlmutter...
# when /^alt(_ht)?$/
#     out = alternate_n(n0, n1, 1)
# when /^alt1/
#     out = alternate_n(n0, n1, 1)
# when /^alt2/
#     out = alternate_n(n0, n1, 2)
# when /^alt4/
#     out = alternate_n(n0, n1, 4)
# when /^alt8/
#     out = alternate_n(n0, n1, 8)
else
    raise "Invalid mode #{mode}"
end

raise "Non-unique cpu entries" unless out.size == out.uniq.size

puts
puts 'NUMACTL_CPUS = ' + out.join(',')
puts
puts "note: there are #{out.size} cpus listed in total."

# cpu_binding_config.rb  0-15,32-47 16-31,48-63 seq