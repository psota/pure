// Author: James Psota
// File:   direct_channel_batcher_manual.h 

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

#ifndef DIRECT_CHANNEL_BATCHER_MANUAL_H
#define DIRECT_CHANNEL_BATCHER_MANUAL_H

#include "pure.h"

/*
[ ] barrier for use of this so no thread uses it before initialization
[ ] generalize payload bytes using arguments
[ ] sense initializer calls



*/

using namespace PureRT;

static const bool initial_sense     = 0;
static const int  leader_thread_num = 0;

struct PaddedDoneFlag {
    // start off as not done
    alignas(CACHE_LINE_BYTES) std::atomic<bool> done_flag;
    char pad0[CACHE_LINE_BYTES - sizeof(int)];

    PaddedDoneFlag() {
        // start this as not the initial sense as we are starting as "not done"
        done_flag.store(!initial_sense);
    }
    ~PaddedDoneFlag() = default;
};

class DirectChannelBatcherManual {

  public:
    DirectChannelBatcherManual(int num_parts, EndpointType endpoint_type,
                               int count_per_part, MPI_Datatype datatype,
                               PureThread* calling_pure_thread,
                               int calling_pure_rank, int mate_pure_rank,
                               int bytes_per_datatype, int tag);

    ~DirectChannelBatcherManual();
    void* getBuf(int thread_num);
    void  Enqueue(int thread_num, bool& thread_sense);
    void  EnqueueWait(int thread_num, bool& thread_sense);
    void  Dequeue(int thread_num, bool& thread_sense);
    void  DequeueWait(int thread_num, bool& thread_sense);

  private:
    // TODO: alignment / false sharing issues
    DirectMPIChannel*      dmc;
    BundleChannelEndpoint* endpoint;
    void*                  agg_buf;
    PaddedDoneFlag*        done_flags;
    const int              num_parts;
    const int              bytes_per_datatype;
    const int              payload_bytes;
    const int              total_count;
    const int              tag;
    const MPI_Datatype     datatype;

    const EndpointType endpoint_type;

    alignas(CACHE_LINE_BYTES) std::atomic<bool> init_comm_done_sense =
            initial_sense; // thread sense needs to init to 0 but done flags to
                           // 1
    std::atomic<bool> wait_comm_done_sense =
            initial_sense; // thread wait sense needs to init to 1
};
#endif