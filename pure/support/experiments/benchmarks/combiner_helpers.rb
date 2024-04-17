# Author: James Psota
# File:   combiner_helpers.rb 

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

require 'open3'
require 'colorize'
require 'fileutils'

private def run_cmd!(cmd)
  max_retries = 5
  delay = 1
  success = false

  puts "\tRUNNING:".light_black + " #{cmd}".yellow

  max_retries.times do |try|
    stdout_err, status = Open3.capture2e(cmd)

    if status.success? == false
      warn stdout_err
      $stderr.puts "ERROR: command failed to run. Retrying... (try #{try})".red
      raise "unable to run command #{cmd}." if try >= max_retries

      delay *= 2
      sleep(delay)
    else
      success = true
      puts stdout_err
      break
    end
  end # loop

  if (success == false)
    raise "Unable to run command #{cmd} after #{max_retries}.".red
  end
end

private def run_dir(r)
  f = File.join('..', 'runs', r, 'results.csv')
  f = File.join('..', f) unless File.exist?(f) # try to go up one more level
  f = File.join('..', f) unless File.exist?(f) # try to go up one more level
  f
end

private def get_mode
  valid_modes = %w{paper thesis}

  m = ENV['PURE_GRAPH_MODE']
  raise "You must set the environment variable PURE_GRAPH_MODE to one of #{valid_modes.to_s}" unless valid_modes.include?(m)
  m
end


$TOTAL_LINES = 0

module CombinerHelpers

  def run!(runs, script, name, iters, num_messages_col_name = 'run_args_0', verify_full_data_reporting = true)
    t0 = Time.now

    mode = get_mode
    puts "set NO_BZIP=1 to skip bzipping step; USE_TSV=1 to use existing combined csv file."



    # combine the runs
    if(ENV['USE_TSV'] == '1')
      puts "Using existing combined csv file"
      puts '  *** WARNING: Not combining files. This is if the data was cleaned up manually or if the input files are all older than the combined_results.tsv file.'.red
    else 
      full_run_paths = runs.map { |r| run_dir(r) }
      all_there = check_runs(full_run_paths)
      raise 'Missing some CSVs. Exiting.' unless all_there
      puts "\nCombining #{runs.size} CSVs with R script:".green
      cmd = ENV['CPL'] + '/support/experiments/graph_utils/combine_results.R ' + full_run_paths.join(' ')
      run_cmd! cmd
    end

    # assert size is correct
    expected_size = $TOTAL_LINES - runs.size + 1
    actual_size = `wc -l combined_results.tsv`.strip.to_i

    if(((actual_size.to_f - expected_size.to_f) / expected_size.to_f) > 0.01)
      raise("expected combined_results.tsv to have #{expected_size} but it has #{actual_size}. We allow a small tolerance to account for blank lines, etc.") if actual_size != expected_size
    end

    bzip_data! unless ENV['NO_BZIP'] == '1'

    FileUtils.mkdir_p('old_graphs')
    # cmd = "rm -f *.png *.pdf"
    cmd = "[ '*.png' = \"$(echo *.png)\" ] || mv *.png old_graphs"
    run_cmd! cmd
    cmd = "[ '*.pdf' = \"$(echo *.pdf)\" ] || mv *.pdf old_graphs"
    run_cmd! cmd
    puts 'Moved old graph files to old_graphs...'.yellow

    # $CPL/support/experiments/graph_utils/graph_exp_by_payload_and_num_threads_and_NUMA_v2.R reduce_test combined_results.csv 2021-12-07-11-46-01 3 50000 1
    if(verify_full_data_reporting)
      cmd = ''
    else
      cmd = "VERIFY_REPORTING=0 "
    end

    oldest_timestamp = get_oldest_timestamp(runs)

    cmd += File.join(ENV['CPL'], '/support/experiments/graph_utils', script) + " #{name} combined_results.tsv #{oldest_timestamp} -1 #{iters} 1 #{num_messages_col_name}"
    run_cmd! cmd

    # $CPL/support/experiments/html/display_dir.rb reduce_test $CPL/support/experiments/benchmarks/reduce/runs/2021-12-07-11-46-01 'Iterations: 50000'
    if (ENV["NO_DISPLAY_DIR"]&.to_i == 1)
      puts "Skipping display dir".red
    else
      cmd = ENV['CPL'] + '/support/experiments/html/display_dir.rb ' + name + ' . ' + "'Combined, using #{iters} iterations'" + ' -'
      run_cmd!(cmd)
    end

    FileUtils.mkdir_p(mode)
    cmd = "mv *.png #{mode} ; mv *.pdf #{mode} "
    run_cmd!(cmd)
    puts "Moved generated files to #{mode} directory (as PURE_GRAPH_MODE is set to #{mode}).".yellow

    run_cmd!('git add -f *.pdf')
    run_cmd!("git add -f #{mode}/*.pdf")

    ########################
    total = Time.now - t0
    puts "\nCombining took #{total} seconds overall."
  end

  

  private def check_runs(runs)
    all_there = true
    runs.each do |r|
      if File.exist?(r)
        lines = `wc -l #{r}`.strip.to_i
        $TOTAL_LINES += lines
        raise "Expected more lines in #{r}; is there a problem?" if lines < 2
        puts '[✓]'.green + " EXISTS: #{r}\t(#{lines} lines)"
      else
        puts "[ ] MISSING: #{r}".red
        all_there = false
      end
    end
    all_there
  end

  private def input_data_new?(full_run_paths)
    return true unless File.exist?('combined_results.tsv')

    full_run_paths.each do |f|
      return true if File.mtime(f) > File.mtime('combined_results.tsv')
    end

    false
  end

  private def get_oldest_timestamp(runs)
    runs.sort.last
  end

  private def bzip_data!
    # only bzip if we just regenerated the combined tsv file
    infile = 'combined_results.tsv'
    bzip_file = infile + '.bz2'

    if File.exist?(bzip_file)
      puts "File.mtime(infile): #{File.mtime(infile)}   File.mtime(bzip_file): #{File.mtime(bzip_file)}"
    end
    if File.exist?(bzip_file)
      puts "  should skip regenerating bzip file? #{File.mtime(infile) <= File.mtime(bzip_file)}"
    end

    if File.exist?(bzip_file) && File.mtime(infile) <= File.mtime(bzip_file) &&
      File.mtime('combiner.rb') <= File.mtime(bzip_file)
      puts "Not bzipping data as #{bzip_file} is newer than #{infile}.".yellow
      return
    end

    cmd = "pbzip2 -zkf #{infile}"
    run_cmd!(cmd)
    cmd = "git add -f #{bzip_file}"
    run_cmd!(cmd)
  end
end
