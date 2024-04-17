// Author: James Psota
// File:   recv_bundle_channel.h 

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

#ifndef PURE_TRANSPORT_RECV_BUNDLE_CHANNEL_H
#define PURE_TRANSPORT_RECV_BUNDLE_CHANNEL_H

#pragma once

#include "pure/transport/bundle_channel.h"

class BundleChannel;

class RecvBundleChannel : public BundleChannel {

  private:
    /* This data structure keeps track of when it's ok for a thread to return from
       Wait(). The thread that actually
       receives the data signals to all the other threads when their messages have
       been received. However, note that
       each thread can receive multiple messages. Therefore, a given thread should
       only be signaled (to wake up from Wait)
       when *all* of its messages have been received. We are assuming here that
       the user code only calls wait once after
       *all* of its Dequeues have been issued.

       This data structure keeps track of how many messages each thread expects to
       receive (for each bundle), and how many
       have been received so far.

     */

    // TODO(jim) this is duplicated in SendBundleChannel. Refactor.
    std::unordered_map<int, ConditionFrame*> payloads_available_for_use_map;

  public:
    RecvBundleChannel(int dest_mpi_rank_arg, int total_num_messages_in_bundle_arg, int mpi_message_tag_arg);
    ~RecvBundleChannel() override;
    void BindChannelEndpoint(BundleChannelEndpoint* /*bce*/);
    bool Dequeue(int /*sender_pure_rank*/, int /* dest_pure_rank */);
    bool BufferValid(int /*pure_dest_rank*/);
    void Wait(int /* pure_dest_rank */);
    void SetAllRecvChannelsInSameThreadToWait(int /* pure_rank */);

};

#endif