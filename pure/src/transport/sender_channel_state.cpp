// Author: James Psota
// File:   sender_channel_state.cpp 

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

#include "pure/transport/sender_channel_state.h"
#include "pure/support/helpers.h"

SenderChannelState::SenderChannelState() {
#if DEBUG_CHECK
    sender_state = SenderStateState::invalid;
#endif
}

void SenderChannelState::JustDidInitialize() {
#if DEBUG_CHECK
    assert(sender_state == SenderStateState::invalid);
    sender_state = SenderStateState::initialized;
#endif
}

void SenderChannelState::JustDidEnqueue() {
#if DEBUG_CHECK
    // check(sender_state == SenderStateState::initialized ||
    //               sender_state == SenderStateState::enqueued ||
    //               sender_state == SenderStateState::fully_waited,
    //       "Expected sender_state to be initialized or enqueued or "
    //       "fully_waited, but it is %d");
    check(sender_state == SenderStateState::initialized ||
                  sender_state == SenderStateState::fully_waited,
          "Expected sender_state to be initialized or "
          "fully_waited, but it is %d",
          sender_state);
    sender_state = SenderStateState::enqueued;
#endif
}

void SenderChannelState::JustDidBlockingEnqueue() {
#if DEBUG_CHECK
    assert(sender_state == SenderStateState::initialized ||
           sender_state == SenderStateState::enqueued ||
           sender_state == SenderStateState::fully_waited);
    sender_state = SenderStateState::fully_waited;
#endif
}

void SenderChannelState::JustDidLastWait() {
#if DEBUG_CHECK
    assert(sender_state == SenderStateState::enqueued);
    sender_state = SenderStateState::fully_waited;
#endif
}
