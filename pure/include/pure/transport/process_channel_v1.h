// Author: James Psota
// File:   process_channel_v1.h 

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

#ifndef PURE_TRANSPORT_PROCESS_CHANNEL_V1_H
#define PURE_TRANSPORT_PROCESS_CHANNEL_V1_H

#pragma once

#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <stack>

#include <boost/circular_buffer.hpp>
#include <boost/circular_buffer/space_optimized.hpp>
#include <boost/functional/hash.hpp>

#include "pure/support/condition_frame.h"
#include "pure/support/zed_debug.h"

#include "pure/transport/shared_queue_entry.h"

// note: enabling PROCESS_CHANNEL_DEBUG triggers an innocuous data race in some
// assertion code. This can safely be ignored.
#define PROCESS_CHANNEL_DEBUG (DEBUG_CHECK)
#define PROCESS_CHANNEL_STATS false
#define RELEASE_CIRCULAR_BUF_CAPACITY_CHECK true

using std::mutex;
using std::stack;

// forward declarations
class SendChannel;
class RecvChannel;

namespace process_channel_v1 {

class ProcessChannel {

    static const size_t buffered_payloads_max_size = 4194304; // 2^22

  public:
    explicit ProcessChannel(int /*num_payload_bytes_arg*/);
    ~ProcessChannel();
    void AllocateSendRTBuffer();

    void BindSendChannel(SendChannel* /*sc*/);
    void BindRecvChannel(RecvChannel* /*rc*/);
    void WaitForInitialization();

    bool Enqueue(const size_t /* count */);
    bool EnqueueUserBuf(const buffer_t, const size_t /* count */);
    bool Dequeue();
    void Wait() const;
#ifdef DEBUG_CHECK
    char* GetDescription();
#endif

  private:
    bool receiver_ready_for_copy; // only set in receiver thread
    bool dequeue_completed_recv;

    buffer_t send_rt_buf, recv_rt_buf;

    size_t num_payload_bytes;
    size_t num_unreceived_payloads;

    // note: there is exactly one send_channel and exactly one recv_channel per
    // ProcessChannel
    SendChannel* send_channel;
    RecvChannel* recv_channel;

    // TODO(jim): needed? get working and then return to this.
    mutex           free_buffers_mutex;
    stack<buffer_t> free_buffers;

    mutex buffered_payloads_mutex;
#define USE_SPACE_OPTIMIZED_CIRCULAR_BUFFER false
// saw a 2x slowdown using space-optimized on cagnodes for intra_process_bakeoff
// at 8a2f0831
#if USE_SPACE_OPTIMIZED_CIRCULAR_BUFFER
    boost::circular_buffer_space_optimized<shared_queue_entry_t>
            buffered_payloads;
#else
    boost::circular_buffer<shared_queue_entry_t> buffered_payloads;
#endif

    ConditionFrame initializer_done_initializing_cf;
    ConditionFrame sender_done_direct_copy_cf;

#if PROCESS_CHANNEL_STATS
    size_t stat_num_receives       = 0;
    size_t stat_buffers_malloced   = 0;
    size_t stat_free_bufs_returned = 0;
    size_t stat_free_bufs_gotten   = 0;

    // updated these in Dequeue code only
    size_t stat_case1_direct_recv = 0;
    size_t stat_case2_temp_recv   = 0;
#endif

#ifdef DEBUG_CHECK
    char description[64];
#endif

    inline bool buffered_payloads_is_empty() const {
        return num_unreceived_payloads == 0;
    }
    inline bool buffered_payloads_is_not_empty() const {
        return num_unreceived_payloads > 0;
    }

    inline bool buffered_payloads_is_not_full() const {
        if (num_unreceived_payloads < buffered_payloads.capacity()) {
            return true;
        } else {
            fprintf(stderr, "buffered_payloads_is_not_full: "
                            "num_unreceived_payloads: %d; "
                            "buffered_payloads.capacity(): %d\n",
                    num_unreceived_payloads, buffered_payloads.capacity());
            return false;
        }
    }

    // buffer pool management functions
    buffer_t GetFreeBuf();
    void     ReturnFreeBuf(buffer_t /*buf*/);
    bool     EnqueueImpl(const buffer_t, const size_t /* count */);
};

} // ends namespace process_channel_v1

#endif
