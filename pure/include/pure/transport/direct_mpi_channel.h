// Author: James Psota
// File:   direct_mpi_channel.h 

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

#ifndef PURE_TRANSPORT_DIRECT_MPI_CHANNEL_H
#define PURE_TRANSPORT_DIRECT_MPI_CHANNEL_H

#pragma once

#include "mpi.h"
#include <cmath>
#include <limits>
#include <optional>

#include "pure/common/pure_pipeline.h"
#include "pure/common/pure_rt_enums.h"
#include "pure/support/helpers.h"
#include "pure/support/zed_debug.h"
#include <boost/circular_buffer.hpp>

#ifndef INITIAL_OUTSTANDING_CHANNEL_DEQ_REQ_CLS
#define INITIAL_OUTSTANDING_CHANNEL_DEQ_REQ_CLS 1
#endif

class PureThread;
class RecvChannel;

using namespace PureRT;

class BundleChannelEndpoint;

class DirectMPIChannel {
  private:
    PurePipeline _dequeue_pipeline;
    // TODO: maybe extend this to a vector of enqueue_requests
    // These requests are persistent throughout the life of this channel
    // See https://www.mpi-forum.org/docs/mpi-1.1/mpi-11-html/node51.html for
    // info on MPI persistent communication requests
    boost::circular_buffer<MPI_Request> dequeue_requests;
    MPI_Request                         enqueue_request;

    // TODO: remove this one
    //////////////////////////////////////
    MPI_Request    last_enqueue_request;
    const MPI_Comm _mpi_comm;
    const int      _sender_thread_num_within_world_comm;
    const int      _receiver_thread_num_within_world_comm;
    /////////////////////////////
    PureThread*                _pure_thread  = nullptr;
    RecvChannel*               _recv_channel = nullptr;
    void*                      _rt_buf       = nullptr;
    const PureRT::EndpointType _endpoint_type;
    const int                  _count;
    int                        allocated_size_bytes = 0;
    const MPI_Datatype         _datatype;
    const int                  _mate_mpi_rank_in_comm;
    int                        _encoded_tag;
    const bool                 _using_rt_send_buf_hint;
    const bool                 _using_sized_enqueues_hint;
    const bool                 _using_rt_recv_buf;
    // bool dequeue_in_progress = false;

#if PRINT_PROCESS_CHANNEL_STATS
    // for debugging purposes only
    char     description_[64];
    uint64_t num_mpi_sends            = 0;
    uint64_t num_mpi_recvs            = 0;
    uint64_t num_mpi_enq_waits        = 0;
    uint64_t num_mpi_deq_waits        = 0;
    uint64_t num_work_steal_attempts  = 0;
    int      max_outstanding_deq_reqs = 0;
    int      num_deq_req_resizes      = 0;
#endif
    void EnqueueImpl(buffer_t, const size_t);

  public:
    DirectMPIChannel(BundleChannelEndpoint* /*bce*/, int /*mate_mpi_rank*/,
                     MPI_Comm, int, int, PureRT::EndpointType /*endpoint_type*/,
                     bool /* using_rt_send_buf_hint */, bool, bool,
                     PurePipeline&&);
    ~DirectMPIChannel();

    void Enqueue(const size_t count_to_send);
    void EnqueueUserBuf(const buffer_t, const size_t count_to_send);
    void EnqueueWait(bool                 actually_blocking_wait = true,
                     std::optional<bool*> enqueue_succeeded = std::nullopt);

    void EnqueueBlocking(const size_t count_to_send);
    void EnqueueBlockingUserBuf(const buffer_t, const size_t count_to_send);
    void EnqueueBlockingImpl(const buffer_t sender_buf,
                             const size_t   count_to_send);
    void Dequeue(const buffer_t);
    const PureContRetArr&
         DequeueWait(bool                 actually_blocking_wait = true,
                     std::optional<bool*> dequeue_succeeded = std::nullopt);
    void DequeueBlocking(const buffer_t);

    void       Validate() const;
    static int ConstructTagForMPI(int sender_thread_num_within_world_comm,
                                  int receiver_thread_num_within_world_comm,
                                  int application_tag, MPI_Comm);

#if PRINT_PROCESS_CHANNEL_STATS
    char* GetDescription();
#endif
};
#endif
