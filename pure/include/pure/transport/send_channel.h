// Author: James Psota
// File:   send_channel.h 

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

#ifndef PURE_TRANSPORT_SEND_CHANNEL_H
#define PURE_TRANSPORT_SEND_CHANNEL_H

#pragma once

#include "pure/support/zed_debug.h"
#include "pure/transport/bundle_channel_endpoint.h"
#include "pure/transport/send_to_self_channel.h"
#include "pure/transport/sender_channel_state.h"
#include <fstream>

#define UNSPECIFIED_COUNT_SENTINEL 0

class BundleChannelEndpoint;
class PureThread;
class BatcherManager;
class SendFlatBatcher;

using std::ofstream;

class SendChannel : public BundleChannelEndpoint {

  public:
    // make sure users are not able to access this function call directly
    // TODO(jim): consider using friend class between SendChannel and PureThread
    // to disallow user code from accessing this?
    SendChannel(void* const /*send_buf_arg*/, int /*count_arg*/,
                MPI_Datatype /*datatype_arg*/, int /*sender_pure_rank_arg*/,
                int /*tag_arg*/, int /*dest_pure_rank_arg*/,
                channel_endpoint_tag_t /*channel_endpoint_tag_arg*/,
                ofstream& /*pure_thread_trace_comm_outstream_arg*/,
                bool using_rt_send_buf_hint, PureThread* /* pure_thread_arg */);
    ~SendChannel() override;

    void SetInitializedState();
    int  GetChannelState() const;

    // non-blocking
    void        Enqueue(size_t count_to_send = UNSPECIFIED_COUNT_SENTINEL);
    void        EnqueueUserBuf(const buffer_t,
                               size_t count_to_send = UNSPECIFIED_COUNT_SENTINEL);
    inline void SetEnqueueBatcherManager(BatcherManager* ebm) {
        assert(enqueue_batcher_manager == nullptr);
        enqueue_batcher_manager = ebm;
        channel_type            = PureRT::ChannelEndpointType::DIRECT_BATCHER;
    }

    void Wait();
    bool Test();

    // blocking
    void EnqueueBlocking(size_t count_to_send = UNSPECIFIED_COUNT_SENTINEL);
    void
    EnqueueBlockingUserBuf(const buffer_t,
                           size_t count_to_send = UNSPECIFIED_COUNT_SENTINEL);

    inline void* GetRecvBuf() const override {
        sentinel("Recv buf not available for RecvChannel.") return nullptr;
    }
    inline void SetRecvBuf(void* /*new_recv_buf*/) override {
        sentinel("Recv buf not available for SendChannel.");
    }

    inline void* GetRecvChannelBuf() const {
        sentinel("Recv channel buf not available for SendChannel.");
        return nullptr;
    }

    // Flat buffer methods
    inline void SetSendFlatBatcher(SendFlatBatcher* sfb) {
        assert(flat_batcher == nullptr);
        flat_batcher = sfb;
        channel_type = PureRT::ChannelEndpointType::FLAT_BATCHER;
    }

    inline bool FlatBatcherWaitIsDone() { return flat_buf_wait_done; }

    inline void MarkFlatBatcherWaitDone() {
        assert(flat_buf_wait_done == false);
        flat_buf_wait_done = true;
    }
    inline void ResetFlatBatcherWaitDone() {
        assert(flat_buf_wait_done == true);
        flat_buf_wait_done = false;
    }

  private:
    ofstream&        pure_thread_trace_comm_outstream;
    SendFlatBatcher* flat_batcher            = nullptr;
    BatcherManager*  enqueue_batcher_manager = nullptr;
#if DEBUG_CHECK
    SenderChannelState sender_state;
#endif
    // just the initial state for invariant checking
    bool       flat_buf_wait_done = true;
    const bool using_rt_send_buf_hint;

    void EnqueuePrologue(bool /* mpi_transport */);
    void FlatBatcherEnqueue(void* const user_buf);
    void FlatBatcherEnqueueWait();
    void DirectBatcherEnqueue(void* user_buf);
    void AssertNoSelfSend();
};
#endif
