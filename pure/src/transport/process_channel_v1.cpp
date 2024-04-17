// Author: James Psota
// File:   process_channel_v1.cpp 

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


#include "pure/transport/process_channel_v1.h"
#include "pure/transport/recv_channel.h"
#include "pure/transport/send_channel.h"

namespace process_channel_v1 {

// called either by sender thread or receiver thread via PureProcess. only gets
// called once per sender/receiver pair.
ProcessChannel::ProcessChannel(int num_payload_bytes)
    : receiver_ready_for_copy(false), dequeue_completed_recv(false),
      send_rt_buf(nullptr), recv_rt_buf(malloc(num_payload_bytes)),
      num_payload_bytes(num_payload_bytes), num_unreceived_payloads(0),
      send_channel(nullptr), recv_channel(nullptr),
      buffered_payloads(buffered_payloads_max_size) {}

// called once by PureProcess inside PureProcess destructor
ProcessChannel::~ProcessChannel() {

#if PROCESS_CHANNEL_STATS
    fprintf(stderr,
            "ProcessChannel v1 Deconstructor Stats *****************\n");
    fprintf(stderr, "  num_receives:\t%zu\n", stat_num_receives);
    fprintf(stderr, "  case 1 receives (direct receive):\t%zu [%.2f%%]\n",
            stat_case1_direct_recv,
            (100 * static_cast<float>(stat_case1_direct_recv) /
             static_cast<float>(stat_num_receives)));
    fprintf(stderr, "  case 2 receives (temp receive):\t%zu [%.2f%%]\n",
            stat_case2_temp_recv,
            (100 * static_cast<float>(stat_case2_temp_recv) /
             static_cast<float>(stat_num_receives)));
    fprintf(stderr, "  buffers malloced:\t%zu\n", stat_buffers_malloced);
    fprintf(stderr, "  free bufs returned:\t%zu\n", stat_free_bufs_returned);
    fprintf(stderr, "  free bufs gotten:\t%zu\n", stat_free_bufs_gotten);
    fprintf(stderr, "  buffered payloads (circular buffer) final size: %zu\n",
            buffered_payloads.size());
    fprintf(stderr, "  free buffers stack final size: %zu\n",
            free_buffers.size());
#endif

    if (send_rt_buf != nullptr) {
        free(send_rt_buf);
    }
    free(recv_rt_buf);

    // there's probably a faster / simpler way, but this is not in the critical
    // path so I don't care.
    // some buffer_t (void * pointers) are in both free_buffers and
    // buffered_payloads. So, we first de-dup
    // by using a set, and then just free from the set.
    std::set<buffer_t> deduped_payloads;
    while (!free_buffers.empty()) {
        deduped_payloads.insert(free_buffers.top());
        free_buffers.pop();
    }

    for (auto i = 0; i < buffered_payloads.size(); ++i) {
        deduped_payloads.insert(buffered_payloads[i].buf);
    }

    // now, do the actual freeing from the deduped set of buffers
    for (auto& payload : deduped_payloads) {
        free(payload);
    }

    // Note: there's a memory leak of at least one of the buffers because
    // the freeing of a buffer for Dequeue N doesn't happen until Dequeue N+1
    // that last buffer (at least one) never makes it back onto the free_buffers
    // list. This is a small issue, so punting on this for now. May come back
    // to it if the same issue ends up being there for the new versions
    // of process channel.

} // function ~

// This is called after construction in PureProcess::BindToProcessChannel if
// using the runtime send buffer. Otherwise it's assumed that the user buffer is
// used.
void ProcessChannel::AllocateSendRTBuffer() {
    send_rt_buf = std::malloc(num_payload_bytes);
    if (send_rt_buf == nullptr) {
        throw std::bad_alloc();
    }
}

void ProcessChannel::BindSendChannel(SendChannel* const sc) {

    ts_log_info("ProcessChannel::BindSendChannel address: %p", this);

    // make sure this is called just once because the SendChannel dictates
    // buffer sizes, etc,
    // and this ProcessChannel was already set up in the constructor.
    assert(send_channel == nullptr);
    send_channel = sc;
    send_channel->SetSendChannelBuf(send_rt_buf);
    send_channel->SetInitializedState();
}

void ProcessChannel::BindRecvChannel(RecvChannel* const rc) {

    ts_log_info("ProcessChannel::BindRecvChannel address: %p", this);

    // make sure this is called just once because the RecvChannel dictates
    // buffer sizes, etc,
    // and this ProcessChannel was already set up in the constructor.
    assert(recv_channel == nullptr); // make sure this is called just once
    recv_channel = rc;
    // starts off pointing to dedicated recv_rt_buf. But, can change to use
    // temporary buffers
    // stored in buffered_payloads throughout use of this channel.
    recv_channel->SetRecvChannelBuf(recv_rt_buf);

    // initialize the receiver buffer if receiver initializtion function was set
    // note: should probably do this for the sender buffer now.
    if (recv_channel->HasBufferInitializerFunc()) {
        // initialize just the first buffer slot. the other buffer slots will be
        // written by send.
        recv_channel->InitBuffer(recv_rt_buf);
    }
    recv_channel->SetInitializedState();

    initializer_done_initializing_cf.MarkReadyAndNotifyOne();
}

void ProcessChannel::WaitForInitialization() {
    ts_log_info("ProcessChannel address: %p", this);
    initializer_done_initializing_cf.WaitForReady();
} // function WaitForInitialization

// uses runtime buffer
bool ProcessChannel::Enqueue(const size_t count_to_send) {
    return EnqueueImpl(send_rt_buf, count_to_send);
}

// uses user buffer
bool ProcessChannel::EnqueueUserBuf(const buffer_t user_buf,
                                    const size_t   count_to_send) {
    return EnqueueImpl(user_buf, count_to_send);
}

// Called only by sender thread
// private
bool ProcessChannel::EnqueueImpl(const buffer_t send_buf,
                                 const size_t   count_to_send) {

    if (PROCESS_CHANNEL_DEBUG) {

        if (send_channel != nullptr && recv_channel != nullptr) {
            assert(send_channel->NumPayloadBytes() ==
                   recv_channel->NumPayloadBytes());
        }
    }

    {
        // TODO(jim): is this lock necessary? get working first, and then
        // consider removing.
        std::lock_guard<mutex> buffered_payloads_lock(buffered_payloads_mutex);
        if (receiver_ready_for_copy) {
            /* CASE 1: In this case the receiver is there waiting for a message
               to come in, and there was nothing
               for it to receive. Therefore, copy directly into its buffer.
               Then, set the flag to false,
               and signal the receiver to wake up.
             */
            if (recv_channel->GetReceivedCount() != count_to_send) {
                recv_channel->SetReceivedCount(count_to_send);
            }

            assert(buffered_payloads_is_empty()); // the receiver should have
                                                  // already checked all buffers
            memcpy(recv_rt_buf, send_buf, num_payload_bytes);
            receiver_ready_for_copy = false; // reset state
            sender_done_direct_copy_cf.MarkReadyAndNotifyOne();

            ts_log_info("[pure rank %d] Enqueue via direct copy.",
                        send_channel->GetSenderPureRank());
        } else {
            // CASE 2:
            // copy the receiver payload into buffered_payloads (via temporary
            // buffer)
            buffer_t buffered_payload = GetFreeBuf();
            memcpy(buffered_payload, send_buf, num_payload_bytes);

// TODO: consider adding this in release mode
// hacking -- clean this up
#if DEBUG_CHECK
            if (!buffered_payloads_is_not_full()) {
                fprintf(stderr, "buffered payloads is full. consider "
                                "increasing the capacity from %d\n",
                        buffered_payloads.capacity());
                std::abort();
            }
#endif
            assert(buffered_payloads_is_not_full());

            // construct the struct to insert into the queue
            shared_queue_entry_t sqe(count_to_send, buffered_payload);

            buffered_payloads.push_back(sqe);
            ++num_unreceived_payloads; // necessary bookkeeping to handle case 1
                                       // vs. case 2
            // fprintf(stderr, "Enqueue: cb size: %d\n",
            // buffered_payloads.size());

            ts_log_info("[pure rank %d] Enqueue via buffering.",
                        send_channel->GetSenderPureRank());
        }
    } // unlocks buffered_payloads_lock

    // in line with semantics of BundleChannel::Enqueue
    bool msg_sent = true;
    return msg_sent;

} // function Enqueue

bool ProcessChannel::Dequeue() {

    if (PROCESS_CHANNEL_DEBUG) {
        if (send_channel != nullptr && recv_channel != nullptr) {
            assert(send_channel->NumPayloadBytes() ==
                   recv_channel->NumPayloadBytes());
        }
    }

    {
        // TODO(jim): is this lock necessary? get working first, and then
        // consider removing.
        std::lock_guard<mutex> buffered_payloads_lock(buffered_payloads_mutex);

        if (recv_channel->GetRawRecvChannelBuf() != recv_rt_buf) {
            // the only time the channel's recv buffer isn't equal to
            // recv_rt_buf is when
            // buffered_payloads was used on the previous dequeue. So, return it
            // to the free list for
            // another use (possibly this use). This must be done before
            // re-setting it, of course.
            ReturnFreeBuf(recv_channel->GetRawRecvChannelBuf());
        }

        if (buffered_payloads_is_empty()) {
            // CASE 1

            sender_done_direct_copy_cf.MarkNotReady();

            // fprintf(stderr, "Dequeue: Case 1: cb size: %d\n",
            // buffered_payloads.size());

            // set conditions so message can be received directly into
            // recv_channel buffer
            receiver_ready_for_copy = true;
            dequeue_completed_recv  = false;

            if (recv_channel->GetRawRecvChannelBuf() != recv_rt_buf) {
                recv_channel->SetRecvChannelBuf(recv_rt_buf);
            }
#if PROCESS_CHANNEL_STATS
            ++stat_case1_direct_recv;
#endif

        } else {
            // CASE 2: use buffered payload
      
            const shared_queue_entry_t sqe = buffered_payloads.front();
            buffered_payloads.pop_front();
            --num_unreceived_payloads; // necessary bookkeeping to handle case 1
                                       // vs. case 2

            if (recv_channel->GetReceivedCount() != sqe.payload_count) {
                recv_channel->SetReceivedCount(sqe.payload_count);
            }

            if (recv_channel->GetRawRecvChannelBuf() != sqe.buf) {

                // no memcpy; just update the buffer pointer.
                // memcpy(recv_rt_buf, sender_buffer, num_payload_bytes);
                recv_channel->SetRecvChannelBuf(sqe.buf);

            } else {
                sentinel("This shouldn't happen, right? I'm not 100\% sure "
                         "about this case but it looks like this shouldn't "
                         "happen.");
            }

            dequeue_completed_recv = true;
// don't return this buffer to free buffers list as it's still in use

#if PROCESS_CHANNEL_STATS
            ++stat_case2_temp_recv;
#endif
        }

    } // unlocks buffered_payloads_lock

#if PROCESS_CHANNEL_STATS
    ++stat_num_receives;
#endif
    return dequeue_completed_recv;

} // function Dequeue

void ProcessChannel::Wait() const {

    if (dequeue_completed_recv) {
        // nothing to do, just return
    } else {
        // wait on message to come in from sender via the ConditionFrame
        sender_done_direct_copy_cf.WaitForReady();
        // at this point the message has been received.
    }

} // function Wait

// Called only by sender thread
buffer_t ProcessChannel::GetFreeBuf() {

    buffer_t new_buf;
    if (free_buffers.empty()) {
        // create a new buffer and return that; the receiver is in charge of
        // putting this back onto the free_buffers
        // list
        new_buf = static_cast<buffer_t>(malloc(num_payload_bytes));
#if PROCESS_CHANNEL_STATS
        ++stat_buffers_malloced;
#endif
        ts_log_info("[pure rank %d] GetFreeBuf via malloc. (pool size: %lu)",
                    send_channel->GetSenderPureRank(), free_buffers.size());

    } else {
        // return one of the free buffers
        new_buf = free_buffers.top();
        free_buffers.pop();
        ts_log_info("[pure rank %d] GetFreeBuf via buffer pool (reuse). (pool "
                    "size: %lu)",
                    send_channel->GetSenderPureRank(), free_buffers.size());
    }

#if PROCESS_CHANNEL_STATS
    ++stat_free_bufs_gotten;
#endif
    return new_buf;
}

// Called only by receiver thread
void ProcessChannel::ReturnFreeBuf(buffer_t buf) {
    free_buffers.push(buf);
    ts_log_info("[pure rank %d] Returning buf to pool. (pool size: %lu)",
                recv_channel->GetDestPureRank(), free_buffers.size());
#if PROCESS_CHANNEL_STATS
    ++stat_free_bufs_returned;
#endif
}

#ifdef DEBUG_CHECK
char* ProcessChannel::GetDescription() {
    sprintf(description, "\tr%d -> r%d", send_channel->GetSenderPureRank(),
            recv_channel->GetDestPureRank());
    return description;
};
#endif

} // namespace process_channel_v1
