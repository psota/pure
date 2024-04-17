// Author: James Psota
// File:   send_bundle_channel.h 

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

#ifndef PURE_TRANSPORT_SEND_BUNDLE_CHANNEL_H
#define PURE_TRANSPORT_SEND_BUNDLE_CHANNEL_H

#pragma once

#include "pure/transport/bundle_channel.h"

class BundleChannel;

class SendBundleChannel : public BundleChannel {

  private:
  	mutable std::mutex add_send_message_mutex;
  	mutable std::mutex send_ready_for_me_mutex;
    mutable std::mutex bound_send_channels_mutex;

  	std::set<int> ready_sender_ranks;
  	std::condition_variable send_ready_for_me_cv;

    // TODO(jim) this is duplicated in RecvBundleChannel. Refactor.
    std::unordered_map<int, ConditionFrame*> buffer_available_for_writing_map;

  public:
    SendBundleChannel(int dest_mpi_rank_arg, int total_num_messages_in_bundle_arg,
                      int mpi_message_tag_arg);
    ~SendBundleChannel() override;
    void BindChannelEndpoint(BundleChannelEndpoint* /*bce*/);
    bool Enqueue(int /*sender_pure_rank*/);
    bool BufferWritable(int /*pure_sender_rank*/);
    void WaitUntilBufWritable(int /*pure_sender_rank*/);
};

#endif