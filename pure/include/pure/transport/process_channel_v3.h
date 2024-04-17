// Author: James Psota
// File:   process_channel_v3.h 

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

#ifndef PURE_TRANSPORT_PROCESS_CHANNEL_V3_H
#define PURE_TRANSPORT_PROCESS_CHANNEL_V3_H

#pragma once

#include "pure/support/untyped_producer_consumer_queue.h"
#include "pure/support/zed_debug.h"
#include <vector>

// TODO: turn this back on generally after working through
// TSAN issues
#define PROCESS_CHANNEL_DEBUG (DEBUG_CHECK)
#define PROCESS_CHANNEL_STATS (PRINT_PROCESS_CHANNEL_STATS)
#define PROCESS_CHANNEL_3_BUFFERED_PAYLOADS_ENTRIES \
    (PROCESS_CHANNEL_BUFFERED_MSG_SIZE)

// forward declarations
class SendChannel;
class RecvChannel;

using namespace PureRT;

namespace process_channel_v3 {

// we must align this as CACHE_LINE_BYTES because this is an aggregate class
// which has a UntypedProducerConsumerQueue which has 64 byte alignment.
class alignas(CACHE_LINE_BYTES) ProcessChannel {

    friend class BundleChannelEndpoint;

  public:
    explicit ProcessChannel(int /*num_payload_bytes*/);
    ~ProcessChannel();

    void AllocateSendRTBuffer();
    void BindSendChannel(SendChannel* /*sc*/);
    void BindRecvChannel(RecvChannel* /*rc*/);
    void WaitForInitialization() const;

    void Enqueue(const size_t count_to_send);
    void EnqueueUserBuf(const buffer_t, const size_t count_to_send);
    void EnqueueWait();
    void Dequeue();
    void DequeueWait();

#if PROCESS_CHANNEL_STATS
    void DumpWaitStats(int mpi_rank, FILE* f);
#endif

#ifdef DEBUG_CHECK
    char* GetDescription();
#endif

  private:
    UntypedProducerConsumerQueue buffered_payloads_;
    // note: there is exactly one send_channel and exactly one recv_channel
    // per ProcessChannel
    alignas(CACHE_LINE_BYTES) SendChannel* send_channel_     = nullptr;
    PureThread*  send_pure_thread_ = nullptr;
    buffer_t send_rt_buf_;

    
    const size_t num_payload_bytes_;

    alignas(CACHE_LINE_BYTES) PureThread*  recv_pure_thread_ = nullptr;
    RecvChannel* recv_channel_     = nullptr;


#if PROCESS_CHANNEL_STATS
    size_t stat_enqueue_queue_full_iterations  = 0;
    size_t stat_dequeue_queue_empty_iterations = 0;
    size_t stat_num_enqueues                   = 0;
    size_t stat_num_dequeues                   = 0;
    size_t stat_buffered_payloads_max_sz       = 0;

    std::vector<std::pair<struct timespec, unsigned int>>
            pending_process_recv_channel_snapshots;

#endif
#ifdef DEBUG_CHECK
    char description_[64];
#endif

    void CheckInvariants() const;
    void EnqueueImpl(const buffer_t, const size_t count_to_send);
    void EnqueueSpinWhileFull(const buffer_t sender_buf,
                              const size_t   count_to_send)
#if PROFILE_MODE
            __attribute__((noinline))
#endif
            ;
    void* DequeueSpinWhileEmpty(void* /*sbp*/)
#if PROFILE_MODE
            __attribute__((noinline))
#endif
            ;
};
} // ends namespace

#endif
