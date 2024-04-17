// Author: James Psota
// File:   process_channel_v2.h 

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

#ifndef PURE_TRANSPORT_PROCESS_CHANNEL_V2_H
#define PURE_TRANSPORT_PROCESS_CHANNEL_V2_H

#pragma once

#include "pure/3rd_party/folly/ProducerConsumerQueueAligned.h"
#include "pure/support/zed_debug.h"
#include "pure/transport/shared_queue_entry.h"

//#include "pure/transport/process_channel.h"

#define PROCESS_CHANNEL_DEBUG (DEBUG_CHECK)
#define PROCESS_CHANNEL_STATS (PRINT_PROCESS_CHANNEL_STATS)

// forward declarations
class SendChannel;
class RecvChannel;

/////////////////////////////////////////////////////////////////////

namespace process_channel_v2 {

class ProcessChannel {

    friend class BundleChannelEndpoint;

  public:
    explicit ProcessChannel(int /*num_payload_bytes*/);
    ~ProcessChannel();
    void AllocateSendRTBuffer();

    void BindSendChannel(SendChannel* /*sc*/);
    void BindRecvChannel(RecvChannel* /*rc*/);
    void WaitForInitialization() const;

    bool Enqueue(const size_t /* count */);
    bool EnqueueUserBuf(const buffer_t, const size_t /* count */);
    bool Dequeue();
    void Wait() const;
#ifdef DEBUG_CHECK
    char* GetDescription();
#endif

  private:
    const size_t num_payload_bytes_;
    buffer_t     send_rt_buf_; // buffer that the sending thread always writes
                               // into

#if PROCESS_CHANNEL_STATS
    size_t stat_enqueue_queue_full_iterations  = 0;
    size_t stat_dequeue_queue_empty_iterations = 0;
    size_t stat_num_enqueues                   = 0;
    size_t stat_num_dequeues                   = 0;
    size_t stat_buffers_malloced               = 0;
    size_t stat_free_bufs_returned             = 0;
    size_t stat_free_bufs_gotten               = 0;
    size_t stat_buffered_payloads_max_sz       = 0;
    size_t stat_free_buffers_max_sz            = 0;
#endif

#ifdef DEBUG_CHECK
    char description_[64];
#endif

    // note: there is exactly one send_channel and exactly one recv_channel
    // per
    // ProcessChannel
    SendChannel* send_channel_;
    RecvChannel* recv_channel_;

#if PCV2_OPTION_USE_PADDED_BUF_PTR == 1
    folly::ProducerConsumerQueueAligned<shared_queue_entry_t, true,
                                        CACHE_LINE_BYTES>
            buffered_payloads_;
#if PCV2_OPTION_USE_FREELIST == 1
    folly::ProducerConsumerQueueAligned<buffer_t, true, CACHE_LINE_BYTES>
            free_buffers_;
#endif

#else /* PCV2_OPTION_USE_PADDED_BUF_PTR is 0 */
    folly::ProducerConsumerQueueAligned<shared_queue_entry_t, false>
            buffered_payloads_;
#if PCV2_OPTION_USE_FREELIST == 1
    folly::ProducerConsumerQueueAligned<buffer_t, false> free_buffers_;
#endif
#endif

    void     CheckInvarients() const;
    buffer_t GetFreeBuf();
    void     ReturnFreeBuf(buffer_t /*buf_to_recycle*/);
    bool     EnqueueImpl(const buffer_t, const size_t /* count */);
};
}

#endif
