#!/usr/bin/env ruby

# Author: James Psota
# File:   coder.rb 

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



# Usage: drop your MPI snippet in coder.txt and out comes the pure code

require 'awesome_print'
require 'colorize'
require 'optimist'

opts = Optimist::options do
    opt :wrap_block, "Wrap in block", :short => 'b', :type => :bool, :default => false
end

private def c_dt_from_mpi_dt(c_dt)
    case c_dt
    when /MPI_INT/
        'int'
    when /MPI_FLOAT/
        'float'
    when /MPI_DOUBLE/
        'double'
    when /MPI_LONG_LONG_INT/
        'long long int'
    else
        raise "uninmplemented datatype: #{c_dt}".red
    end
end

private def determine_comm(mpi_comm) 
    mpi_comm == 'MPI_COMM_WORLD' ? 'PURE_COMM_WORLD' : mpi_comm
end

private def gen_send(mpi) 
    m = mpi.match(/MPI_I?[sS]end\((.*)\)/)
    args = m[1].split(/,/)
    args.map!{|e| e.strip }
    c_dt = c_dt_from_mpi_dt(args[2])
    comm = determine_comm(args[5])
    ap args

# TODO: revert
    # <<~HEREDOC
    #     // #{mpi}
    #     const bool using_rt_send_buf   = false;
    #     const bool using_sized_enqueue = true;
    #     SendChannel* const sc = pure_thread_global->GetOrInitSendChannel(
    #             #{args[1]}, #{args[2]}, #{args[3]}, #{args[4]},
    #             using_rt_send_buf, using_sized_enqueue, PROCESS_CHANNEL_BUFFERED_MSG_SIZE, #{comm});
    #     sc->EnqueueUserBuf(#{args[0]}, #{args[1]});
    #     //sc->EnqueueUserBuf(#{args[0]}); // no sized enqueue variation (change above)
    #     sc->Wait();
    # HEREDOC

    <<~HEREDOC
        // #{mpi}
        const bool using_rt_send_buf   = false;
        const bool using_sized_enqueue = false;
        send_channels[i] = pure_thread_global->GetOrInitSendChannel(
                #{args[1]}, #{args[2]}, #{args[3]}, #{args[4]},
                using_rt_send_buf, using_sized_enqueue, PROCESS_CHANNEL_BUFFERED_MSG_SIZE, #{comm});
        send_channels[i]->EnqueueUserBuf(#{args[0]});
        // send_channels[i]->Wait(); // must do this!
    HEREDOC
end

private def gen_recv(mpi)
    m = mpi.match(/MPI_I?[rR]ecv\((.*)\)/)
    args = m[1].split(/,/)
    args.map!{|e| e.strip }
    ap args
    c_dt = c_dt_from_mpi_dt(args[2])
    comm = determine_comm(args[5])

# revert
    # <<~HEREDOC
    #     // #{mpi}
    #     RecvChannel* const rc = pure_thread_global->GetOrInitRecvChannel(
    #             #{args[1]}, #{args[2]}, #{args[3]}, #{args[4]}, false, PROCESS_CHANNEL_BUFFERED_MSG_SIZE, #{comm});
    #     rc->Dequeue(#{args[0]});
    #     rc->Wait();
    # HEREDOC

    <<~HEREDOC
        // #{mpi}
        recv_channels[i] = pure_thread_global->GetOrInitRecvChannel(
                #{args[1]}, #{args[2]}, #{args[3]}, #{args[4]}, false, PROCESS_CHANNEL_BUFFERED_MSG_SIZE, #{comm});
        recv_channels[i]->Dequeue(#{args[0]});
        recv_channels[i]->Wait();
    HEREDOC

end

private def gen_sendrecv(mpi) 
    m = mpi.match(/MPI_Sendrecv\((.*)\)/)
    args = m[1].split(/,/)
    args.map!{|e| e.strip }
    ap args
    send_c_dt = c_dt_from_mpi_dt(args[2])
    recv_c_dt = c_dt_from_mpi_dt(args[7])

    # Important: initialize receiver first due to quirk in initialization of ProcessChannels.
    <<~HEREDOC
        // #{mpi}
        bool channel_init;
        RecvChannel* const rc = pure_thread_global->GetOrInitRecvChannel(#{args[6]}, #{args[7]}, #{args[8]}, #{args[9]}, false, channel_init);
        SendChannel* const sc = pure_thread_global->GetOrInitSendChannel(#{args[1]},#{args[2]}, #{args[3]}, #{args[4]}, channel_init, false, true);
        pure_send_recv_user_bufs(sc, #{args[0]}, #{args[1]}, rc, #{args[5]});
        sc->Wait();
        rc->Wait();
    HEREDOC
end

#         MPI_Bcast(params, 37, MPI_INT, 0, MPI_COMM_WORLD);
private def gen_bcast(mpi) 
    m = mpi.match(/MPI_Bcast\((.*)\)/)
    args = m[1].split(/,/)
    args.map!{|e| e.strip }
    c_dt = c_dt_from_mpi_dt(args[2])
    ap args

    <<~HEREDOC
        // #{mpi}
        const auto __my_rank = FIXME;
        const auto __cet = FIXME;
        bcast_single_val<#{c_dt}>(pure_thread_global, __my_rank, #{args[0]}, #{args[2]}, __cet, #{args[1]}, #{args[3]});
    HEREDOC
end


private def gen_allreduce(mpi)
    m = mpi.match(/MPI_Allreduce\((.*)\)/)
    args = m[1].split(/,/)
    args.map!{|e| e.strip }
    c_dt = c_dt_from_mpi_dt(args[3])
    ap args

=begin
 ripping this out for amr
         new_arc = new AllReduceChannel(
                pure_thread_global, #{args[2]}, #{args[3]}, #{args[4]},
                R_CET_FIXME, B_CET_FIXME);
=end

    <<~HEREDOC
        // #{mpi}
        ar->AllReduce(pure_thread_global, PURE_COMM_WORLD, #{args[0]}, #{args[1]}, #{args[2]}, #{args[3]}, #{args[4]}, profile_ar_seq);
    HEREDOC
end

##########################################################
private def execute(pure_code, opts)
    final = opts[:wrap_block] ? "{\n#{pure_code}\n}" : pure_code

    puts "\n" + final + "\n"
    system "echo '#{final}' | pbcopy"
    puts "Pure code copied to clipboard.".green
end
##########################################################

mpi = IO.read("coder.txt")
raise "Blank coder.txt file" unless mpi.size > 0

mpi.gsub!(/\n/, ' ')
mpi.strip!
mpi.squeeze!(' ')

###################################################

if(mpi.include?('MPI_Send') || mpi.include?('MPI_Isend'))
    execute(gen_send(mpi), opts)
elsif(mpi.include?('MPI_Recv') || mpi.include?('MPI_Irecv'))
    execute(gen_recv(mpi), opts)
elsif(mpi.include?('MPI_Sendrecv'))
    execute(gen_sendrecv(mpi), opts)
elsif(mpi.include?('MPI_Allreduce'))
    execute(gen_allreduce(mpi), opts)
elsif(mpi.include?('MPI_Bcast'))
    execute(gen_bcast(mpi), opts)
else
    raise "NYI"
end
