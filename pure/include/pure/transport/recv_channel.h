// Author: James Psota
// File:   recv_channel.h 

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

#ifndef PURE_TRANSPORT_RECV_CHANNEL_H
#define PURE_TRANSPORT_RECV_CHANNEL_H

#pragma once

#include "pure/common/pure_pipeline.h"
#include "pure/support/zed_debug.h"
#include "pure/transport/bundle_channel_endpoint.h"
#include "pure/transport/experimental/direct_channel_batcher.h"
#include "pure/transport/recv_channel_state.h"
#include <atomic>
#include <fstream>

class BundleChannelEndpoint;
class RecvChannelState;
class PureThread;
class RecvFlatBatcher;
class BatcherManager;

class RecvChannel : public BundleChannelEndpoint {

  public:
    // make sure users are not able to access this function call directly
    // TODO(jim): consider using friend class between SendChannel and PureThread
    // to disallow user
    // code from accessing this?
    RecvChannel(void* const /*recv_buf_arg*/, int /*count_arg*/,
                const MPI_Datatype& /*datatype_arg*/,
                int /*sender_pure_rank_arg*/, int /*tag_arg*/,
                int /*dest_pure_rank_arg*/,
                channel_endpoint_tag_t /*channel_endpoint_tag_arg*/,
                std::ofstream& /*pure_thread_trace_comm_outstream_arg*/,
                PureRT::buffer_init_func_t init_func,
                PureThread* /* pure_thread_arg */);

    ~RecvChannel() override;

    void SetInitializedState();
    void SetWaitedState();

    int GetChannelState() const;

    void Dequeue(const buffer_t user_buf = nullptr);
    void DequeueBlocking(const buffer_t user_buf = nullptr);
    void SetDequeueBatcherManager(BatcherManager* dbm);

    inline void SetRecvFlatBatcher(RecvFlatBatcher* rfb) {
        assert(recv_flat_batcher == nullptr);
        recv_flat_batcher = rfb;
        channel_type      = PureRT::ChannelEndpointType::FLAT_BATCHER;
    }

    bool                  BufferValid();
    const PureContRetArr& Wait();
    bool                  Test();
    static void           WaitAll(int count, RecvChannel* const* const rcs,
                                  PureThread* const);
    // static int            WaitAny(int count, RecvChannel* const* const rcs,
    //                               PureThread* const pure_thread, bool*
    //                               completed, int& remaining);
    static int WaitAny(int count, RecvChannel* const* const rcs,
                       PureThread* const        pure_thread,
                       std::list<RecvChannel*>& remain_rcs);

    // returns count for most recent dequeue
    inline size_t GetReceivedCount() const { return received_count; }
    void          WaitForAllMyRanksEndpoints();
    bool          HasBufferInitializerFunc() const;
    void          InitBuffer(void* /*buf*/);

    inline void* GetSendBuf() const override {
        sentinel("Send buf not available for RecvChannel.") return nullptr;
    }
    inline void SetSendBuf(void* /*new_send_buf*/) override {
        sentinel("Send buf not available for RecvChannel.")
    }
    inline void* GetSendChannelBuf() const {
        sentinel("Send channel buf not available for "
                 "RecvChannel.") return nullptr;
    }

    inline void SetReceivedCount(const size_t new_received_count) {
        received_count = new_received_count;
    }

#if FLAT_BATCHER_RECV_VERSION == 1
    // flat batcher routines
    inline void ResetFlatBatcherMsgCompleted() {
        flat_batcher_msg_complete.store(false, std::memory_order_release);
    }
    inline void MarkFlatBatcherMsgCompleted() {
        flat_batcher_msg_complete.store(true, std::memory_order_release);
    }
#endif
#if FLAT_BATCHER_RECV_VERSION == 2
    inline void ResetFlatBatcherMsgCompleted() {
        flat_batcher_msg_complete = false;
    }
    inline void MarkFlatBatcherMsgCompleted() {
        flat_batcher_msg_complete = true;
    }
    inline bool FlatBatcherMsgCompleted() { return flat_batcher_msg_complete; }
#endif

    inline void SetIndex(int i) {
        // only allow set once
        assert(waitany_idx = -1);
        waitany_idx = i;
    }
    inline auto GetIndex() const { return waitany_idx; }
    void        Validate() const;

  private:
    long long unsigned int direct_channel_batcher_recv_ticket = 0;
#if DEBUG_CHECK
    RecvChannelState receiver_state;
#endif
    std::ofstream&                   pure_thread_trace_comm_outstream;
    RecvFlatBatcher*                 recv_flat_batcher       = nullptr;
    BatcherManager*                  dequeue_batcher_manager = nullptr;
    const PureRT::buffer_init_func_t buf_init_func = nullptr; // optional
    void*                            flat_batcher_temp_user_recv_buf = nullptr;
    void*  direct_batcher_temp_user_recv_buf                         = nullptr;
    size_t received_count                                            = 0;

    // hack for waitany -- see if this can work and then clean up
    int waitany_idx = -1;

#if FLAT_BATCHER_RECV_VERSION == 1
    atomic<bool> flat_batcher_msg_complete = true;
#endif
#if FLAT_BATCHER_RECV_VERSION == 2
    // initially marked false by the recv call
    bool flat_batcher_msg_complete = true;
#endif

    ///////////////////////
    void WaitImplementation(bool /*apply_to_all_threads_endpoints*/);
    void WaitForFlatBatcherCompletion() const;
    void DirectBatcherRecv();
};
#endif
