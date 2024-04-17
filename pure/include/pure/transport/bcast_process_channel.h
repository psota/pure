// Author: James Psota
// File:   bcast_process_channel.h 

// Copyright (c) 2024 James Psota
// __________________________________________

// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
// 
//   http://www.apache.org/licenses/LICENSE-2.0
// 
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#ifndef BCAST_PROCESS_CHANNEL_H
#define BCAST_PROCESS_CHANNEL_H

#include "mpi.h"
#include "pure/common/pure_rt_enums.h"
#include "pure/support/benchmark_timer.h"
#include "pure/support/sp_bcast_queue.h"
#include "pure/transport/bcast_channel.h"
#include <atomic>
#include <optional>

using PureRT::buffer_t;

class BcastChannel;
class PureThread;

class BcastProcessChannel {
  public:
    BcastProcessChannel(int count_arg, MPI_Datatype datatype_arg,
                        int root_mpi_rank_in_comm_arg, int size_bytes_arg,
                        int  root_pure_rank_arg,
                        int  num_threads_this_process_comm_arg,
                        bool is_multiprocess_arg, bool is_root_process_arg,
                        MPI_Comm mpi_comm_arg);

    ~BcastProcessChannel();
    void BindBcastChannel(BcastChannel* const bc);

    void BcastBlocking(PureThread* pure_thread, int calling_pure_rank,
                       int thread_num_in_pure_comm, buffer_t buf,
                       uint32_t& consumer_last_index, uint32_t& thread_seq,
                       std::optional<bool> force_process_leader,
                       std::optional<bool> skip_cross_process_bcast);

  private:
    // leader only vars
    alignas(CACHE_LINE_BYTES) std::atomic<void*> read_only_process_leader_buf;
    std::atomic<uint32_t> bcast_data_ready_seq = -1;
    int                   done_consuming_flag;

    alignas(CACHE_LINE_BYTES) SingleProducerBcastQueue queue;
    alignas(CACHE_LINE_BYTES) PaddedAtomicFlag* done_consuming_flags;

    // read-only variables
    alignas(CACHE_LINE_BYTES) MPI_Comm mpi_comm;
    const MPI_Datatype datatype;
    const int          root_pure_rank_in_pure_comm;
    const int          num_threads_this_process_comm;
    const int          size_bytes;
    const int          count;
    const int          root_mpi_rank_in_comm;
    const bool         pure_comm_is_multiprocess;
    const bool         is_root_process;

    char pad1[CACHE_LINE_BYTES - (sizeof(MPI_Comm) + sizeof(MPI_Datatype) +
                                  5 * sizeof(int) + 2 * sizeof(bool))];

    /////////////////////////////////////////
    void LeaderQueueBcast(PureThread* pt, void const* const __restrict src_buf);
    void NonLeaderQueueRecvBcast(PureThread* pt,
                                 void* const __restrict dest_buf, int,
                                 uint32_t&);
    bool ElectProcessLeader(int            calling_pure_rank,
                            const uint64_t thread_leader_ticket_num);
    // void InvertThreadSense(bool&);

    bool DoQueueStyleBcast();
    void LeaderDirectCopyBcast(PureThread* pure_thread, uint32_t,
                               void* const __restrict src_buf);

    void NonLeaderDirectCopyBcast(PureThread* const pure_thread,
                                  uint32_t          thread_seq,
                                  void* const __restrict dest_buf,
                                  int thread_num_in_pure_comm);
};

#endif