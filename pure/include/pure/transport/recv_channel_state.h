// Author: James Psota
// File:   recv_channel_state.h 

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

#ifndef PURE_TRANSPORT_RECV_CHANNEL_STATE
#define PURE_TRANSPORT_RECV_CHANNEL_STATE

#include "pure/support/helpers.h"

/* TODO: incorporate getting valid buffer pointer */

enum class RecvStateState {
    /*0*/ invalid,
    /*1*/ initialized,
    /*2*/ dequeued_not_usable_buffer,
    /*3*/ usable_buffer
};

class RecvChannelState {
  private:
    RecvStateState receiver_state;


    // TODO: incorporate count of outstanding_dequeues and make sure that stays reasonable. maybe the waits always bring it back to zero before it goes up again?

  public:
    RecvChannelState();
    ~RecvChannelState() = default;

    void JustDidInitialize();
    void JustDidDequeue();
    void JustDidBlockingDequeue();
    void JustDidWait();

    inline void AssertUsableBuffer() const { 
        assert(receiver_state == RecvStateState::usable_buffer);
    }

    inline int ToInt() const { return as_integer<RecvStateState>(receiver_state); }
};

#endif