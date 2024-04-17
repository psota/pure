#!/usr/bin/env ruby

# Author: James Psota
# File:   gdb_thread_stack_stats.rb 

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
require 'action_view'
include ActionView::Helpers::NumberHelper
require 'byebug'

# add something like this to the makefile to dump backtraces to gdb. By default, these stacks go to gdb.txt
# extend to other wait symbols as well (bcast, reduce, barrier, etc.)

=begin

define USER_GDB_COMMANDS
break ProcessChannel::DequeueWaitSpinWhileEmpty
commands
silent
thread apply all bt
cont 200
end

break EnqueueWaitBlockingThisOnly
commands
silent
thread apply all bt
cont 200
end
endef

=end

# sample line from file:

# #0  0x0000000000251a80 in std::_Rb_tree<int, std::pair<int const, int>, std::_Select1st<std::pair<int const, int> >, std::less<int>, std::allocator<std::pair<int const, int> > >::_S_value (__x=0xb5ac50) at /data/scratch/jim/toolchains/gcc-8.3.0/lib/gcc/x86_64-pc-linux-gnu/8.3.0/../../../../include/c++/8.3.0/bits/stl_tree.h:772

DEBUG_MODE = false
TIMELINE_NAME = 'stack_timeline.txt'.freeze
STATS_NAME = 'stack_symbol_stats.txt'.freeze
COINCIDENT_STATS_NAME = 'stack_symbol_coincidences.tsv'.freeze
WAIT_SYMS_REGEX = /DequeueWaitSpinWhileEmpty|EnqueueWaitBlockingThisOnly/.freeze

class StackDetail
  attr_accessor :matching_sym, :waiting

  def initialize(matching, waiting)
    @matching_sym = matching
    @waiting = waiting
  end

  def waiting?
    @waiting == true
  end
end

private def print_wait_context(h) 

    ap h
    delim = "\t"

    File.open(COINCIDENT_STATS_NAME, 'w') do |log|
        all_syms = []
        h.each do |break_sym, coincident_syms|
            all_syms += coincident_syms.keys
        end

        all_syms = all_syms.uniq.sort
        puts "ALL SYMS"
        ap all_syms

        log.puts "WAIT SYMBOL" + delim + all_syms.join(delim)
        h = h.delete_if{ |k,v| k.nil? }.sort_by { |k, v| k }.to_h

        h.each do |break_sym, coincident_syms|
            log.print "#{break_sym}\t"
            all_syms.each do |sym|
                log.print "#{coincident_syms[sym] || '0'}\t"
            end
            log.print "\n"
        end
        log.puts
    end
end

private def main(file_filter, num_threads, f)

    inside_new_backtrace = false # inside a thread's backtrace, searching for top-most symbol
    found_topmost_sym = false
    thread_counter = num_threads
    timeline_out = File.open(TIMELINE_NAME, 'w')
    symbol_counter = {}
    num_broken_stacks = 0

    debug_log = File.open("gdb_debug.log", 'w')  if DEBUG_MODE

    # maps from the top-most symbol matching the file filter for the thread that's waiting, 
    # to counts for the symbols for the other threads that are running at the time of the said wait
    stack_details = []
    wait_sym_to_context_syms = {}
    this_backtrace_waiting = false

    #File.readlines(f).each do |line|
    cmd = "wc -l #{f}"
    total_lines = `#{cmd}`.split(' ').first.to_i
    cnt = 0

    IO.foreach(f) do |line|
        if(inside_new_backtrace)
            if(!found_topmost_sym)
                matcher = line.match /#\d+ (0x.* in)?(.*) \(.* at (.*):(\d+)/
                # puts "**** #{line.strip} --> #{matcher}".yellow
                
                debug_log.puts(line) if DEBUG_MODE


                if(matcher)
                    sym         = matcher[2]
                    # possibly clean up sym further
                    sym_matcher = sym.match(/0x.* in (.*)/)
                    if(sym_matcher)
                        sym = sym_matcher[1]
                    end
                    sym.strip!
                    source_file = matcher[3]
                    line_no     = matcher[4]

                    if(sym.match(WAIT_SYMS_REGEX))
                        this_backtrace_waiting = true
                        #puts "#{sym} matched waiting".green
                    else 
                        debug_log.puts "  ***** #{sym} didn't matched waiting!" if DEBUG_MODE
                    end

                    # does this line match the file filter?
                    if(source_file.match(file_filter))
                        # we found it. log the occurance of the symbol
                        symbol_counter[sym] ||= 0
                        symbol_counter[sym] += 1

                        stack_details << StackDetail.new(sym, this_backtrace_waiting)
                        this_backtrace_waiting = false # reset

                        # log in timeline
                        timeline_out.puts "#{sym}   \t#{source_file}\t#{line_no}"
                        thread_counter -= 1

                        if(thread_counter == 0)
                            timeline_out.puts "___________________"
                            thread_counter = num_threads  # reset

                            # we've seen all the threads in this section, so update wait_sym_to_context_syms 
                            #puts "STACK DETAILS:".cyan
                            #ap stack_details

                            # first, find the waiting StackDetail
                            waiting_thread_sym = nil
                            stack_details.each do |sd|
                                if(sd.waiting?)
                                    waiting_thread_sym = sd.matching_sym
                                    #puts "  FOUND WAITING SYM! #{waiting_thread_sym}".yellow
                                    break
                                end
                            end
                            if waiting_thread_sym.nil?
                                puts "\nCURRENT LINE: #{line}"
                                ap stack_details

                                debug_log.puts(stack_details.awesome_inspect) if DEBUG_MODE
                                #debug_log.close

                                #    raise "Didn't find a single waiting thread in this section (#{waiting_thread_sym}). See debug log."
                                puts "Didn't find a single waiting thread in this section (#{waiting_thread_sym}). See debug log.".red
                                num_broken_stacks += 1
                                waiting_thread_sym = "Unknown"
                            end

                            #puts "  waiting sym is #{waiting_thread_sym}"
                            wait_sym_to_context_syms[waiting_thread_sym] ||= {}
                            stack_details.each do |sd|
                                if(sd.waiting?)
                                    # don't count the waiting one
                                    next
                                end
                                wait_sym_to_context_syms[waiting_thread_sym][sd.matching_sym] ||= 0
                                wait_sym_to_context_syms[waiting_thread_sym][sd.matching_sym] += 1
                            end

                            # reset structures
                            stack_details = []
                        end

                        found_topmost_sym = true
                        inside_new_backtrace = false
                    end # if found top-most matching symbol
                end
            end
        else
            # top-most matching, like this:   Thread 4 (Thread 0x7ffff50d9700 (LWP 39252)):
            matcher = line.match /Thread (\d+) \(Thread 0x.* \(LWP \d+\)\):/
            if (matcher)
                inside_new_backtrace = true
                found_topmost_sym = false # reset
            end
        end

        cnt += 1
        if (cnt % 1000000 == 0)
            pct = (cnt.to_f / total_lines.to_f * 100).round(1)
            puts "  - finished #{number_with_delimiter(cnt)} / #{number_with_delimiter(total_lines)}\t#{pct}% done"
        end
    end

    timeline_out.close

    # now log symbol stats
    File.open(STATS_NAME, 'w') do |f|
        symbol_counter.sort_by{|_,v| -v}.each do |k,v|
            f.puts "#{number_with_delimiter(v)}\t#{k}"
        end
    end

    print_wait_context(wait_sym_to_context_syms)

    puts "  See output files:"
    puts "* #{TIMELINE_NAME}"
    puts "* #{STATS_NAME}"
    puts "* #{COINCIDENT_STATS_NAME}"
    puts

    if(num_broken_stacks > 0)
        $stderr.puts "\nMalformed stacks occured #{num_broken_stacks} / #{number_with_delimiter(total_lines)} times.".red
    end

    debug_log.close if DEBUG_MODE

end

###################################
raise "ERROR: Usage: #{$0} <file_filter> <num_threads> <gdb_input> \n\nThis tool takes the top-most symbol that's in the given file_filter. (In the future, this can be extended to support multiple files.\n" unless ARGV.size >= 1
file_filter = ARGV[0]
num_threads = ARGV[1].to_i
raise "invalid num_threads" unless num_threads > 0
f = ARGV[2] || 'gdb.txt'
main(file_filter, num_threads, f)
