// Author: James Psota
// File:   bcast_channel.cpp 

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

#include "pure/transport/bcast_channel.h"
#include "pure/transport/bcast_process_channel.h"

BcastChannel::BcastChannel(int count_arg, const MPI_Datatype& datatype_arg,
                           int caller_pure_rank_in_pure_comm_arg,
                           int thread_num_in_process_comm_arg,
                           int root_pure_rank_in_pure_comm_arg,
                           channel_endpoint_tag_t channel_endpoint_tag_arg,
                           PureThread*            pure_thread_arg)
    : BundleChannelEndpoint(nullptr, nullptr, count_arg, datatype_arg,
                            root_pure_rank_in_pure_comm_arg,
                            PureRT::PURE_UNUSED_TAG, PureRT::PURE_UNUSED_RANK,
                            channel_endpoint_tag_arg, pure_thread_arg),
      pure_rank_in_pure_comm(caller_pure_rank_in_pure_comm_arg),
      thread_num_in_process_comm(thread_num_in_process_comm_arg) {}

void BcastChannel::Bcast(const PureRT::buffer_t buf,
                         std::optional<bool>    force_process_leader,
                         std::optional<bool>    skip_cross_process_bcast) {
    bcast_process_channel->BcastBlocking(
            GetPureThread(), pure_rank_in_pure_comm, thread_num_in_process_comm,
            buf, my_consumer_read_index, thread_seq, force_process_leader,
            skip_cross_process_bcast);
}
