// Author: James Psota
// File:   rdma_mpi_channel.h 

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

#ifndef RDMA_MPI_CHANNEL_H
#define RDMA_MPI_CHANNEL_H

#include "mpi.h"
#include "pure/common/pure_rt_enums.h"
#include "pure/common/pure_pipeline.h"

using PureRT::buffer_t;

#define PURE_RDMA_MODE_FENCE 0
#define PURE_RDMA_MODE_PSCW 1
#define PURE_RDMA_MODE_LOCK 2

class BundleChannelEndpoint;
class MPICommManager;

class RdmaMPIChannel {

  public:
    RdmaMPIChannel(BundleChannelEndpoint* const bce,
                   const MPICommManager& comm_mgr, int my_mpi_rank,
                   int partner_mpi_rank, PureRT::EndpointType endpoint_type,
                   PurePipeline&&);
    ~RdmaMPIChannel();
    void Free();

    void Enqueue(const buffer_t sender_buf, const size_t count_to_send);
    void EnqueueWait();

    void Dequeue(buffer_t);
    const PureContRetArr& DequeueWait();

  private:
    PurePipeline dequeue_pipeline_;
    buffer_t buf_;
    MPI_Win      win_;
    MPI_Group    chan_group_, origin_or_target_group_;
    MPI_Comm     chan_comm_;
    int          partner_chan_comm_rank_;
    const int    count_;
    const MPI_Datatype datatype_;
};

#endif