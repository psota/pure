// Author: James Psota
// File:   all_reduce_channel.h 

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

#ifndef ALL_REDUCE_CHANNEL_H
#define ALL_REDUCE_CHANNEL_H

#include "pure/support/helpers.h"
#include "pure/transport/bcast_channel.h"
#include "pure/transport/reduce_channel.h"

class PureThread;

class AllReduceChannel {

  public:
    AllReduceChannel(PureThread* const pure_thread_arg, int count_arg,
                     MPI_Datatype datatype_arg, MPI_Op op,
                     channel_endpoint_tag_t red_cet,
                     channel_endpoint_tag_t bcast_cet,
                     PureComm* const        pc = nullptr);

    // buffers are given by user at reduction time; therefore, the same
    // channel can be used at multiple call sites.
    ~AllReduceChannel() = default;

    template <typename ValType>
    inline void AllReduce(ValType* const local_reduce_buf,
                          ValType* const result_buf) {

        const bool do_allreduce             = true;
        const bool skip_cross_process_bcast = true;
        bool       became_process_leader;

#if DEBUG_CHECK && MPI_ENABLED
        // make sure ValType corresponds to datatype
        int mpi_size;
        int ret = MPI_Type_size(datatype, &mpi_size);
        assert(ret == MPI_SUCCESS);
        assert(sizeof(ValType) == mpi_size);
#endif

        reduce_channel->Reduce(local_reduce_buf, result_buf,
                               &became_process_leader, do_allreduce);
        bcast_channel->Bcast(result_buf, became_process_leader,
                             skip_cross_process_bcast);
    }

    inline int GetCount() const { return count; }

  private:
    PureThread* const pure_thread;
    ReduceChannel*    reduce_channel;
    BcastChannel*     bcast_channel;

    const int          root_rank_in_comm;
    const int          count;
    const MPI_Datatype datatype;
};

#endif