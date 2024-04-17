#!/usr/bin/env ruby

# Author: James Psota
# File:   user_source_to_pure.rb 

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



# TODO:
#   [ ] warn or error when it's a C program that already uses extern "C". it'll probably break as it is now.
#   [ ] make this fail if main is not found. then, make the other programs in the chain above this fail as well.
#   [ ] instead of commenting out old code, use #ifdef to allow old code to be run.

require 'fileutils'
require 'optimist'
require 'colorize'
require 'open3'

opts = Optimist::options do
	opt :input, "Input file to process", :type => :string, :required => true, :short => 'i'
	opt :pure_suffix, "Suffix to append to base file name", :type => :string, :default => '.purified'
end

raise "input file must end in .cpp or .c" unless /(.*)\.(c(pp)?)$/.match(opts[:input])
raise "input file #{opts[:input]} can't be found" unless File.exist? opts[:input]

input_file_base = $1
filename_ext = $2

# IMPORTANT: keep this variable name in sync with what it's called in pure_user.h
pure_thread_name = "pure_thread".freeze

def output_filename(opts, input_file_base, filename_ext)
	# NOTE: the suffixes logged and not-logged must be kept in sync with the definition of 
	# PURIFIED_SRCS_NAME_SUFFIX, which is defined in Makefile.include.mk
	log_suffix = ENV['PURIFIED_SRCS_NAME_SUFFIX']
	raise "Invalid value for environment variable PURIFIED_SRCS_NAME_SUFFIX. Expected logged or not-logged but was #{log_suffix}" unless ['logged', 'not-logged'].include?(log_suffix)
	return "#{input_file_base}.#{log_suffix}#{opts[:pure_suffix]}.#{filename_ext}"
end

def enable_thread_logger?
	ENV['ENABLE_THREAD_LOGGER'] && ENV['ENABLE_THREAD_LOGGER'].to_i == 1
end

private def prepend_space_match_entry(str, matcher_space_index)
	str.split("\n").map{|l| "\\#{matcher_space_index}#{l}"}.join("\n")
end

private def prepend_comment(str)
	str.split("\n").map{|l| "//#{l}"}.join("\n")
end

C_TYPE_REPLACMENT_PLACEHOLDER = "C_TYPE_REPLACMENT_PLACEHOLDER".freeze

class Conversion 

	def initialize(name, matcher, replacement, c_type_replacement_with_matcher_idx=nil)
		@name = name
		@matcher = matcher
		@replacement = replacement
		@num_replaces = 0
		@c_type_replacement_with_matcher_idx = c_type_replacement_with_matcher_idx
	end

	def match? str
		return @matcher.match str
	end

	def purify str
		@num_replaces += 1
		subbed = str.sub @matcher, @replacement
		if @c_type_replacement_with_matcher_idx
			mpi_dt = (str.match @matcher)[@c_type_replacement_with_matcher_idx]
			c_dt = c_pointer_type_from_mpi_datatype(mpi_dt)
			subbed.gsub!(C_TYPE_REPLACMENT_PLACEHOLDER, c_dt)
		end
		subbed
	end

	def stats_string
		return "#{@name}: converted #{@num_replaces} time#{@num_replaces != 1 ? 's' : ''}"
	end

	private def c_pointer_type_from_mpi_datatype(mpi_dt)
		case mpi_dt
			when /MPI_INT/
				"int*"
			when /MPI_FLOAT/
				"float*"
			when /MPI_DOUBLE/
				"double*"
			when /MPI_CHAR/
				"char*"
			else
				raise "Unsupported MPI_Datatype #{mpi_dt} in #{$0}. You probably need to add an entry to c_pointer_type_from_mpi_datatype"
			end
	end

end

CONVERSIONS = []

main_replacement_str = "int __orig_main( int argc, char* argv[], PureThread* #{pure_thread_name} ) {"
if(enable_thread_logger?) 
	main_replacement_str += "
  /* Set up ThreadLogger system */
  ThreadLogger::initialize(#{pure_thread_name});
"
else
	main_replacement_str += "\n/* Note: ThreadLogger disabled. Set ENABLE_THREAD_LOGGER = 1 to enable. */"
end

# TODO: change this regex for main to also match the **argc variant.
main_conversion = Conversion.new("main", /int\s+main\s*\(\s*int argc,\s*char\s*\*\s*argv\[\]\s*\)\s*{/, main_replacement_str)
CONVERSIONS << main_conversion

# http://www.mpich.org/static/docs/v3.1/www3/MPI_Init.html
# note: we are removing both the C++ and C versions of the API calls, as they may be mixed and matched.
repl = ''

# reminder: use multiline regexes for any string that could possibly wrap across multiple lines (anything non-trivially short)

CONVERSIONS << Conversion.new("MPI_Init", /MPI_Init\s*\(\s*&argc\s*,\s*&argv\s*\)\s*;/, repl)
CONVERSIONS << Conversion.new("MPI::Init", /MPI::Init\s*\(\s*argc\s*,\s*argv\s*\)\s*;/, repl)
CONVERSIONS << Conversion.new("MPI_Comm_rank", 
	                          /MPI_Comm_rank\s*\(\s*MPI_COMM_WORLD\s*,\s*&(.*)\)\s*;/, 
	                          "\\1 = #{pure_thread_name}->Get_rank();")

CONVERSIONS << Conversion.new("MPI::COMM_WORLD.Get_rank", 
	                          /MPI::COMM_WORLD.Get_rank\(\)/, 
	                          "#{pure_thread_name}->Get_rank();")

CONVERSIONS << Conversion.new("MPI_Comm_size", 
	                          /MPI_Comm_size\s*\(\s*MPI_COMM_WORLD\s*,\s*&(.*)\)\s*;/, 
	                          "\\1 = #{pure_thread_name}->Get_size();")

CONVERSIONS << Conversion.new("MPI::COMM_WORLD.Get_size", /MPI::COMM_WORLD.Get_size\(\)/, "#{pure_thread_name}->Get_size();")

# MPI_Send -- only inserting comments for now to force user to modify
# can only have spaces in front of it, not anything else such as comment lines.
send_regex = /^(\s*)MPI_Send\s*\((.*),(.*),(.*),(.*),(.*),(.*)\s*\)\s*;/m
# key
#   \1 space before send (for indenting)
#   \2 buf
#   \3 count
#   \4 datatype
#   \5 dest rank
#   \6 tag
#   \7 comm
pure_send_replace = <<-SEND_REPL

// TODO: replace __send_buf and __send_channel with your send channel's name
#{C_TYPE_REPLACMENT_PLACEHOLDER} __send_buf;
SendChannel* __send_channel = nullptr;
if(__send_channel == nullptr) {
	__send_channel = pure_thread->InitSendChannel(nullptr, \\3, \\4, \\5, \\6, BundleMode::none);
	__send_channel->WaitForInitialization();
}

OPTION 1: RUNTIME BUFFER (Recommended)
__send_buf = __send_channel->GetSendChannelBuf<#{C_TYPE_REPLACMENT_PLACEHOLDER}>();
__send_channel->WaitUntilBufWritable();
assert(__send_channel->BufferWritable());
// TODO: write some data into the buffer
// ex: __send_buf[0] = 5;
__send_channel->Enqueue();

OPTION 2: USER BUFFER
__send_channel->WaitUntilBufWritable(); /* just for state management; get rid of this for process channel */
assert(__send_channel->BufferWritable()); /* just for state management; get rid of this for process channel */
// assume buffer already written by this point, or write it
__send_channel->EnqueueUserBuf(\\2);

SEND_REPL
space_idx = 1
pure_send_replace = prepend_space_match_entry(pure_send_replace, space_idx)
pure_send_replace = prepend_comment(pure_send_replace)

datatype_idx = 4
CONVERSIONS << Conversion.new("MPI_Send", send_regex, pure_send_replace, datatype_idx)

# MPI_Recv -- only inserting comments for now to force user to modify
recv_regex = /^(\s*)MPI_Recv\s*\((.*),(.*),(.*),(.*),(.*),(.*),(.*)\s*\)\s*;/m
# key
#   \1 space before send (for indenting)
#   \2 buf
#   \3 count
#   \4 datatype
#   \5 dest rank
#   \6 tag
#   \7 comm
#   \8 status
pure_recv_replace = <<-RECV_REPL
// TODO: replace __recv_buf and __recv_channel with your recv channel's name
#{C_TYPE_REPLACMENT_PLACEHOLDER} __recv_buf;
RecvChannel* __recv_channel = nullptr;
if(__recv_channel == nullptr) {
	__recv_channel = pure_thread->InitRecvChannel(\\2, \\3, \\4, \\5, \\6, BundleMode::none);
	__recv_channel->WaitForInitialization();
}
__recv_channel->Dequeue();
__recv_buf = __recv_channel->GetRecvChannelBuf<#{C_TYPE_REPLACMENT_PLACEHOLDER}>();
__recv_channel->Wait();
assert(__recv_channel->BufferValid());
// __recv_buf can also be gotten again here, or if you need to read the buffer, read it now (not before!).

// buffer now valid for reading and writing (verified for process channels, but not for BundleChannels)
// read and write payload buffer
// int val     = recv_buf[0];
// recv_buf[0] = val + 1;
RECV_REPL
space_idx = 1
pure_recv_replace = prepend_space_match_entry(pure_recv_replace, space_idx)
pure_recv_replace = prepend_comment(pure_recv_replace)

datatype_idx = 4
CONVERSIONS << Conversion.new("MPI_Recv", recv_regex, pure_recv_replace, datatype_idx)

barrier_replace = <<-BARRIER_REPL
/* PURE_TODO: Consider replace barrier with Pure barrier. Note, however that MPI_Barrier is threadsafe, so it's not necessary to replace it. */
MPI_Barrier(MPI_COMM_WORLD);
BARRIER_REPL
barrier_replace = prepend_space_match_entry(barrier_replace, space_idx)
CONVERSIONS << Conversion.new("MPI_Barrier", 
	                          /^(\s*)MPI_Barrier\s*\(\s*MPI_COMM_WORLD\s*\)\s*;/m, 
	                          barrier_replace)

reduce_replace = <<-REDUCE_REPL
/* PURE_TODO: Consider replacing reduce with Pure equivalent. Note, however that MPI_Barrier is threadsafe, so it's not necessary to replace it. */
MPI_Reduce(\\2);
REDUCE_REPL
reduce_replace = prepend_space_match_entry(reduce_replace, space_idx)

CONVERSIONS << Conversion.new("MPI_Reduce", 
	                          /^(\s*)MPI_Reduce\s*\((.*)\)\s*;/m, 
	                          reduce_replace)

allreduce_replace = <<-ALL_REDUCE_REPL
/* PURE_TODO: Consider replace allreduce with Pure equivalent. Note, however that MPI_Barrier is threadsafe, so it's not necessary to replace it. */
MPI_Allreduce(\\2);
ALL_REDUCE_REPL
allreduce_replace = prepend_space_match_entry(allreduce_replace, space_idx)

CONVERSIONS << Conversion.new("MPI_Allreduce", 
	                          /^(\s*)MPI_Allreduce\s*\((.*)\)\s*;/m, 
	                          allreduce_replace)

# just comment out Finalize
repl = ''
CONVERSIONS << Conversion.new("MPI_Finalize", /MPI_Finalize\s*\(\s*\);/, repl)
CONVERSIONS << Conversion.new("MPI::Finalize", /MPI::Finalize\s*\(\s*\);/, repl)
CONVERSIONS.freeze

i=1

infile_string = nil
outfile_string_arr = []

# clang-format a copy of the input file to create long lines
input_file_copy_name = opts[:input] + '.copy'
FileUtils.rm_f input_file_copy_name
FileUtils.cp(opts[:input], input_file_copy_name)


# temp skipping!
#system("clang-format -style=\"{BasedOnStyle: llvm, ColumnLimit: 2000}\" -i #{input_file_copy_name}") or raise "Failed to run clang-format on #{input_file_copy_name}"
File.open(input_file_copy_name) do |f|
	infile_string = f.read
end

outfile_string_arr << "// PURE: automatically added by Pure source-to-source translation"
outfile_string_arr << "#include \"pure.h\""
outfile_string_arr << "using namespace PureRT;"
outfile_string_arr << ''

file_contains_main = false
infile_string.each_line do |line|

	# string matching approach: white we loop through each line, multi-line regexes 
	# created with /m regex modifiers, will work because the following lines are checked even
	# though the line starts with the line variable.

	new_line = line.rstrip
	did_conversion = false
	CONVERSIONS.each do |c|

		# special check for main
		if(c == main_conversion && c.match?(new_line))
			file_contains_main = true
		end

		if(c.match? new_line)
			new_line = c.purify(new_line)
			did_conversion = true
		end
	end

	if did_conversion
		# determine indentation
		/^(\s*).*/.match line
		indentation_spaces = $1
		outfile_string_arr << "\n#{indentation_spaces}/* BEFORE PURE CONVERSION: #{line.strip} */"
	end
	outfile_string_arr << new_line
	i += 1

end

out_string = outfile_string_arr.join("\n")

# STEP 2. location-specific conversions
# STEP 2.1. replace only the last exit(...) from main at the end of main()

# THIS IS REALLY BRITTLE. main has to be at end of file, doesn't match properly. just return 0 at end of main.

if file_contains_main

	exit_from_main_regex = /(int\s+__orig_main\(.*\).*{.*)(^\s*)exit\s*\((.+)\);(.*)/m
	exit_from_main_subst = "\\1"
	if enable_thread_logger?
		exit_from_main_subst += "\n\\2/* Clean up ThreadLogger system */"
		exit_from_main_subst += "\\2ThreadLogger::finalize();\n" 
	end
	exit_from_main_subst += "\\2return \\3;
\\2/* BEFORE PURE CONVERSION: exit(\\3); */
\\4"

	# hack: clean this up.
	# raise "\nsource-to-source translation: Error: failed to find exit_from_main_regex. Exiting...".colorize(:red) unless out_string.match(exit_from_main_regex) 
	out_string.gsub!( exit_from_main_regex, exit_from_main_subst )

end

# STEP 3. write the file with some stats
output_fname = output_filename(opts, input_file_base, filename_ext)
File.open(output_fname, 'w') do |outfile|

	outfile.write out_string

	outfile.puts "\n\n\n\n\n/*\n\nFinished processing #{opts[:input]} (output in #{output_filename(opts, input_file_base, filename_ext)}). Stats:"
	CONVERSIONS.each do |c|
		outfile.puts c.stats_string
	end	

	outfile.puts "\n*/"
end

# STEP 4. clang-format the generated file

# temp skipping!
#cmd = "clang-format -i #{output_fname}"

#cmd_out, status = Open3.capture2e(cmd)
# if !status.success?
# 	raise "#{cmd} returned with error code #{status.to_s}"
# end

# clean up input copy
# FileUtils.rm_f input_file_copy_name
