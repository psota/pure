// Author: James Psota
// File:   reduce_channel.h 

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

#ifndef PURE_TRANSPORT_REDUCE_CHANNEL_H
#define PURE_TRANSPORT_REDUCE_CHANNEL_H

#pragma once

#include "pure/transport/bundle_channel_endpoint.h"
#include <atomic>

class BundleChannelEndpoint;
class ReduceProcessChannel;
class PureThread;
class PureComm;

class ReduceChannel : public BundleChannelEndpoint {

  public:
    ReduceChannel(int count_arg, const MPI_Datatype& datatype_arg,
                  int sender_pure_rank_in_comm, int root_pure_rank_in_comm_arg,
                  channel_endpoint_tag_t channel_endpoint_tag_arg,
                  MPI_Op mpi_op_arg, PureThread* pure_thread_arg,
                  PureComm const* const thread_pure_comm_arg);
    ~ReduceChannel() override = default;

    void WaitForInitialization() const override;
    void Reduce(buffer_t src_buf, buffer_t recv_buf,
                bool*               is_process_leader = nullptr,
                std::optional<bool> do_allreduce      = std::nullopt);

    ////////////////
    void          SetReduceProcessChannel(ReduceProcessChannel* const rpc);
    void*         GetBuf() const;
    void          ClearBuf();
    inline MPI_Op GetOp() const { return mpi_op; }

    // for updating num_consumed
    void Wrapup(int);
    void ResetConsumption();
    void MarkDoneConsumption();

  private:
    alignas(CACHE_LINE_BYTES) uint64_t leader_ticket_num = 0;
    PureComm const* const thread_pure_comm;
    ReduceProcessChannel* reduce_process_channel = nullptr;
    const MPI_Op          mpi_op;
    const int             pure_rank_in_comm;
    bool                  thread_sense = true;

    ///////////////////////////////////////////
    alignas(CACHE_LINE_BYTES) std::atomic<buffer_t> buf = nullptr;
    std::atomic<int> num_consumed                       = 0;
    char             end_pad[CACHE_LINE_BYTES - sizeof(std::atomic<buffer_t>) -
                 sizeof(std::atomic<int>)];

    ///////////////////////////////////////////
    void StoreBuf(buffer_t b);
    void WaitForAllConsumption(int);
};

#endif
