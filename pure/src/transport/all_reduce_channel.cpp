// Author: James Psota
// File:   all_reduce_channel.cpp 

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

#include "pure/transport/all_reduce_channel.h"
#include "pure/runtime/pure_thread.h"

AllReduceChannel::AllReduceChannel(PureThread* const pure_thread_arg,
                                   int count_arg, MPI_Datatype datatype_arg,
                                   MPI_Op op, channel_endpoint_tag_t red_cet,
                                   channel_endpoint_tag_t bcast_cet,
                                   PureComm* const        pc)
    // note: the 0 root rank is ignored and leaders are chosen dynamically. We
    // are choosing a real rank, though, so other sanity checks throughout pass.
    : pure_thread(pure_thread_arg), root_rank_in_comm(0), count(count_arg),
      datatype(datatype_arg) {

    assert(pure_thread_arg != nullptr);

    if (DEBUG_CHECK && count == 1 && op == MPI_SUM && datatype == MPI_DOUBLE) {
        sentinel("Use the special AllReduceSumOneDoubleProcess for this "
                 "instead.");
    }

    // TODO: consider a more sophisticated algorithm. See
    // https://towardsdatascience.com/visual-intuition-on-ring-allreduce-for-distributed-deep-learning-d1f34b4911da
    // Right now Pure allreduce seems to be slower than MPI allreduce
    reduce_channel = pure_thread->InitReduceChannel(
            count, datatype, root_rank_in_comm, op, red_cet, pc);
    reduce_channel->WaitForInitialization();

    bcast_channel = pure_thread->InitBcastChannel(
            count, datatype, root_rank_in_comm, bcast_cet, pc);
}
