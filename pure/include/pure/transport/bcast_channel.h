// Author: James Psota
// File:   bcast_channel.h 

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

#ifndef PURE_TRANSPORT_BCAST_CHANNEL_H
#define PURE_TRANSPORT_BCAST_CHANNEL_H

#include "mpi.h"
#include "pure/common/pure_rt_enums.h"
#include "pure/transport/bcast_process_channel.h"
#include "pure/transport/bundle_channel_endpoint.h"
#include <optional>

class BundleChannelEndpoint;
class BcastProcessChannel;
class PureThread;

class BcastChannel : public BundleChannelEndpoint {

  public:
    BcastChannel(int count_arg, const MPI_Datatype& datatype_arg,
                 int                    caller_pure_rank_in_pure_comm_arg,
                 int                    thread_num_in_process_comm,
                 int                    root_pure_rank_in_pure_comm_arg,
                 channel_endpoint_tag_t channel_endpoint_tag_arg,
                 PureThread*            pure_thread_arg);
    ~BcastChannel() override = default;

    inline void SetBcastProcessChannel(BcastProcessChannel* const bpc) {
        assert(bcast_process_channel == nullptr);
        bcast_process_channel = bpc;
    }

    void Bcast(const PureRT::buffer_t buf,
               std::optional<bool>    force_process_leader     = std::nullopt,
               std::optional<bool>    skip_cross_process_bcast = std::nullopt);

  private:
    BcastProcessChannel* bcast_process_channel  = nullptr;
    uint32_t             my_consumer_read_index = 0; // must be zero
    uint32_t             thread_seq             = 0;
    const int            pure_rank_in_pure_comm;
    int                  thread_num_in_process_comm;
};

#endif
