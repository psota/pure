// Author: James Psota
// File:   channel_state.h 

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

enum class ChannelSenderState {
    /*0*/ invalid,
    /*1*/ initialized,
    /*2*/ have_buffer_waiting_until_writable,
    /*3*/ writable,
    /*4*/ enqueued_not_writable
};

enum class ChannelReceiverState {
    /*0*/ invalid,
    /*1*/ initialized,
    /*2*/ have_unusable_buffer /* unreadable, and unwritable */,
    /*3*/ waiting_for_readable_writable,
    /*4*/ readable_writable
};

class ChannelState {
  private:
    ProcessChannelSenderState   sender_state;
    ProcessChannelReceiverState receiver_state;

  public:
    ChannelState();
    ~ChannelState() = default;

    void IncrementSenderState();
    void IncrementReceiverState();

    void VerifySenderState(ProcessChannelSenderState s1,
                           ProcessChannelSenderState s2 = ProcessChannelSenderState::invalid) const;
    void VerifyReceiverState(ProcessChannelReceiverState s1,
                             ProcessChannelReceiverState s2 = ProcessChannelReceiverState::invalid) const;
};
