// Author: James Psota
// File:   recv_channel_state.cpp 

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

#include "pure/transport/recv_channel_state.h"
#include "pure/support/helpers.h"
#include "pure/support/zed_debug.h"

// Note: these states are for the recv_channel only. Special coordination is
// needed for the Wait case. see recv_channel_diagram.ai figure.

RecvChannelState::RecvChannelState() {
    receiver_state = RecvStateState::invalid;
}

void RecvChannelState::JustDidInitialize() {
    assert(receiver_state == RecvStateState::invalid);
    receiver_state = RecvStateState::initialized;
}

void RecvChannelState::JustDidDequeue() {
    // either coming from initialized state (on very first dequeue) or dequeue
    // (for many dequeues at once) or wait
    check(receiver_state == RecvStateState::initialized ||
                  receiver_state ==
                          RecvStateState::dequeued_not_usable_buffer ||
                  receiver_state == RecvStateState::usable_buffer,
          "Expected other state for dequeue but it was %d", receiver_state);
    receiver_state = RecvStateState::dequeued_not_usable_buffer;
}

void RecvChannelState::JustDidBlockingDequeue() {
    // either coming from initialized state (on very first dequeue) or dequeue
    // (for many dequeues at once) or wait
    assert(receiver_state == RecvStateState::initialized ||
           receiver_state == RecvStateState::dequeued_not_usable_buffer ||
           receiver_state == RecvStateState::usable_buffer);
    receiver_state = RecvStateState::usable_buffer;
}

void RecvChannelState::JustDidWait() {
    check(receiver_state == RecvStateState::dequeued_not_usable_buffer ||
                  receiver_state == RecvStateState::usable_buffer,
          "Just tried to update the RecvChannelState to the usable_buffer state"
          " and expected the receiver state to be in the "
          "dequeued_not_usable_buffer "
          "state or usable_buffer state (for subsequent dequeues) but it was "
          "state %d instead",
          receiver_state);
    receiver_state = RecvStateState::usable_buffer;
}
