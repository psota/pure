#!/usr/bin/env ruby

# Author: James Psota
# File:   mpi_runtime_tracer_annotate.rb 

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




# CSV Format: MPI call, sender rank, receiver rank, payload size, datatype, tag, datatype_bytes

require 'optimist'
require 'colorize'
require 'awesome_print'
require 'fileutils'
require 'byebug'

def main
	opts = Optimist::options do
		opt :input, "Input file to process", :type => :string, :required => true, :short => 'i'
		opt :annotated_src_suffix, "Suffix to append to base file name", :type => :string, :default => '.traced'
	end

	annotator_outfile_var = '__annotator_outfile'.freeze
	annotator_my_rank_var = '__annotator_my_rank'.freeze

	raise "input file #{opts[:input]} can't be found" unless File.exist? opts[:input]

	matcher = opts[:input].match /(.*)\.(.*)/
	basename = matcher[1]
	ext = matcher[2]
	raise "input file must end in .cpp or .c" unless (ext == 'c' || ext == 'cpp')


	outfile_name = basename + opts[:annotated_src_suffix] + '.' + ext
	outfile = File.open(outfile_name, 'w')

	puts "MPI TRACER INFO: tracing input file #{opts[:input]}, creating #{outfile_name}".cyan

	global_init = <<-GLOBALS_HEREDOC
////////////// AUTOMATICALLY INSERTED BY MPI TRACER
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include "pure/support/colors.h"

int   __annotator_my_rank;
FILE* __annotator_outfile;
int   __temp_datatype_size = -1;
////////////// END - AUTOMATICALLY INSERTED BY MPI TRACER
	GLOBALS_HEREDOC

	outfile.puts global_init

	post_mpi_init_code = <<-INIT_CODE_HEREDOC
    ////////////// AUTOMATICALLY INSERTED BY MPI TRACER
    printf(KRED "WARNING: RUNNING WITH MPI TRACER.\\n" KRESET);

    char outfile_name[64];
    sprintf(outfile_name, "logs/__annotator_outfile_%d.csv", getpid());

#ifdef __cplusplus
    __annotator_outfile = std::fopen(const_cast<char*>(outfile_name), "w");
#else
    __annotator_outfile = fopen(outfile_name, 'w');
#endif

    if (!__annotator_outfile) {
        fprintf(stderr, "Opening annotator outfile %s failed\\n", outfile_name);
        return EXIT_FAILURE;
    }
    MPI_Comm_rank(MPI_COMM_WORLD, &__annotator_my_rank);

    ////////////// END - AUTOMATICALLY INSERTED BY MPI TRACER
	INIT_CODE_HEREDOC

	done_post_mpi_init_setup = false

	# create copy of file to work on that deals with lines of CPP code going across lines. We clean this up later.
	# note that we don't change the formatting on the original source

	input_file_copy_name = opts[:input] + '.copy'
	FileUtils.rm_f input_file_copy_name
	FileUtils.cp(opts[:input], input_file_copy_name)
	system("clang-format -style=\"{BasedOnStyle: llvm, ColumnLimit: 2000}\" -i #{input_file_copy_name}") or raise "Failed to run clang-format on #{input_file_copy_name}"

	# loop through file
	File.open(input_file_copy_name) do |f|

		f.each_line do |line|

			outfile.puts line  # print original line
			# MPI_Send(send_buf, payload_count, MPI_INT, recv_first_rank, msg_tag, MPI_COMM_WORLD);
			m = line.match(/(\s*)MPI_Send\s*\((.*),(.*),(.*),(.*),(.*),(.*)\s*\)\s*;/m)

			if m
				logger_print = <<-EOS
#{m[1]}MPI_Type_size(#{m[4]}, &__temp_datatype_size);
#{m[1]}fprintf(#{annotator_outfile_var}, "%s:%d,%s,MPI_Send,%d,%d,%d,%d,%d,%d\\n", __FILE__, __LINE__-2, __FUNCTION__, #{annotator_my_rank_var}, #{m[5]}, #{m[3]}, #{m[4]}, #{m[6]}, __temp_datatype_size);
				EOS
				outfile.puts logger_print
			end

			#  MPI_Isend(Work_out_p, Block_size, MPI_DOUBLE, send_to, phase, MPI_COMM_WORLD, &send_req);
			m = line.match(/(\s*)MPI_Isend\s*\((.*),(.*),(.*),(.*),(.*),(.*),(.*)\s*\)\s*;/m)
			if m
				logger_print = <<-EOS
#{m[1]}MPI_Type_size(#{m[4]}, &__temp_datatype_size);
#{m[1]}fprintf(#{annotator_outfile_var}, "%s:%d,%s,MPI_Isend,%d,%d,%d,%d,%d,%d\\n", __FILE__, __LINE__-2, __FUNCTION__, #{annotator_my_rank_var}, #{m[5]}, #{m[3]}, #{m[4]}, #{m[6]}, __temp_datatype_size);
				EOS
				outfile.puts logger_print
			end
			puts line

			# MPI_Recv(recv_buf, payload_count, MPI_INT, recv_first_rank, msg_tag, MPI_COMM_WORLD, &req);

			# CSV Headers: file:line, function, MPI function, receiver rank, my rank, 
			m = line.match(/(\s*)MPI_Recv\s*\((.*),(.*),(.*),(.*),(.*),(.*),(.*)\s*\)\s*;/m)
			if m
				logger_print = <<-EOS
#{m[1]}MPI_Type_size(#{m[4]}, &__temp_datatype_size);
#{m[1]}fprintf(#{annotator_outfile_var}, "%s:%d,%s,MPI_Recv,%d,%d,%d,%d,%d,%d\\n", __FILE__, __LINE__-2, __FUNCTION__, #{m[5]}, #{annotator_my_rank_var}, #{m[3]}, #{m[4]}, #{m[6]}, __temp_datatype_size);
				EOS
				#puts logger_print
				outfile.puts logger_print
			end

			# MPI_Irecv(Work_in_p, Block_size, MPI_DOUBLE, recv_from, phase, MPI_COMM_WORLD, &recv_req);
			m = line.match(/(\s*)MPI_Irecv\s*\((.*),(.*),(.*),(.*),(.*),(.*),(.*)\s*\)\s*;/m)
			if m
				logger_print = <<-EOS
#{m[1]}MPI_Type_size(#{m[4]}, &__temp_datatype_size);
#{m[1]}fprintf(#{annotator_outfile_var}, "%s:%d,%s,MPI_Irecv,%d,%d,%d,%d,%d,%d\\n", __FILE__, __LINE__-2, __FUNCTION__, #{m[5]}, #{annotator_my_rank_var}, #{m[3]}, #{m[4]}, #{m[6]}, __temp_datatype_size);
				EOS
				outfile.puts logger_print
			end

			# MPI_Init(&argc, &argv);
			m = line.match /MPI_Init\(.*\)\s*;/
			if m && !done_post_mpi_init_setup
				outfile.puts post_mpi_init_code
				done_post_mpi_init_setup = true
			end

			# exit
			m = line.match /(\s*)exit\(0\)\s*;/
			if m
				# erase already-printed line, then annotate, then write line again
				outfile.seek(-(line.size), IO::SEEK_END)
				space = m[1]
				outfile.puts <<-CLEANUP_HEREDOC
#{space}// AUTOMATICALLY INSERTED BY MPI TRACER
#{space}fclose(__annotator_outfile);
#{space}printf(KRED "WARNING: RUNNING WITH MPI TRACER. Only for Send/Recv (not reduce, etc.). See logs/#{annotator_outfile_var}*\\n" KRESET);
#{space}// END - AUTOMATICALLY INSERTED BY MPI TRACER
				CLEANUP_HEREDOC
				outfile.puts line
			end

		end
	end

	outfile.close

	# now, re-clang-format the output file with reasonable line lengths
	system('clang-format', '-i', outfile_name) or raise "Failed to clang-format traced output file #{outfile_name}."
	FileUtils.rm input_file_copy_name
	puts "Done. See #{outfile_name}"

end


if $0 == __FILE__
	main
end
