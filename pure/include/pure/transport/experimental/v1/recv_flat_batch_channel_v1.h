// Author: James Psota
// File:   recv_flat_batch_channel_v1.h 

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

#ifndef RECV_FLAT_BATCH_CHANNEL_V1_H
#define RECV_FLAT_BATCH_CHANNEL_V1_H

#include "mpi.h"
#include "pure/common/pure_rt_enums.h"

class RecvFlatBatchChannel {

  public:
    RecvFlatBatchChannel(int mate_mpi_rank_arg);
    ~RecvFlatBatchChannel();
    bool IsMPIRecvDone(void**, int*);
    void StartRecv();

  private:
    void*       batch_buf;
    MPI_Request recv_req;
    int         received_bytes;
    int         size_bytes_max;
    const int   mate_mpi_rank;
    const int   batch_tag         = PureRT::PURE_DIRECT_CHAN_BATCHER_TAG;
    bool        irecv_outstanding = false;
};

#endif