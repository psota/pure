// Author: James Psota
// File:   sender_channel_state.h 

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

#ifndef PURE_TRANSPORT_SEND_CHANNEL_STATE
#define PURE_TRANSPORT_SEND_CHANNEL_STATE

/* TODO: incorporate getting valid buffer pointer */
#include "pure/support/helpers.h"

enum class SenderStateState {
    /*0*/ invalid,
    /*1*/ initialized,
    /*2*/ enqueued,
    /*3*/ fully_waited
};

class SenderChannelState {
  private:
    SenderStateState sender_state;
    void
    VerifySenderState(SenderStateState s1,
                      SenderStateState s2 = SenderStateState::invalid) const;

  public:
    SenderChannelState();
    ~SenderChannelState() = default;

    void JustDidInitialize();
    void JustDidEnqueue();
    void JustDidBlockingEnqueue();
    void JustDidLastWait(); // no more pending dequeues

    inline int ToInt() const {
        return as_integer<SenderStateState>(sender_state);
    }
};

#endif