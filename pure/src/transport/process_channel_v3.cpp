// Author: James Psota
// File:   process_channel_v3.cpp 

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

#include "pure/transport/process_channel_v3.h"
#include "pure/runtime/pure_thread.h"
#include "pure/transport/recv_channel.h"
#include "pure/transport/send_channel.h"

namespace process_channel_v3 {

// called either by sender thread or receiver thread via PureProcess. only gets
// called once per sender/receiver pair.
ProcessChannel::ProcessChannel(int num_payload_bytes_arg)
    : num_payload_bytes_(num_payload_bytes_arg), send_rt_buf_(nullptr),
      buffered_payloads_(PROCESS_CHANNEL_3_BUFFERED_PAYLOADS_ENTRIES,
                         num_payload_bytes_arg) {}

// called once by PureProcess inside PureProcess destructor
ProcessChannel::~ProcessChannel() {

    if (send_rt_buf_ != nullptr) {
        std::free(send_rt_buf_);
    }

    size_t sz0 = buffered_payloads_.sizeGuess();
    check(sz0 == 1,
          "Upon finishing using this ProcessChannel (v3) the "
          "buffered_payloaded SPSC queue should have just a single entry in "
          "it, assuming the number of receives exactly matched the "
          "number of sends. However, there are %zu entries on "
          "it. It's 1 and not 0 because the popFront call only gets made on a "
          "subsequent Dequeue.",
          buffered_payloads_.sizeGuess());

#if PROCESS_CHANNEL_STATS
    fprintf(stderr,
            "ProcessChannel v3 Deconstructor Stats * (pcv: %p) "
            "**************\n",
            this);
    fprintf(stderr,
            "  Enqueue iterations where queue was full: %zu (%zu "
            "enqueue calls total)\n",
            stat_enqueue_queue_full_iterations, stat_num_enqueues);
    fprintf(stderr,
            "  ~Max buffered payloads queue size: %zu (capacity: %zu "
            "(%.2f%%))\n",
            stat_buffered_payloads_max_sz,
            PROCESS_CHANNEL_3_BUFFERED_PAYLOADS_ENTRIES,
            percentage<int>(stat_buffered_payloads_max_sz,
                            PROCESS_CHANNEL_3_BUFFERED_PAYLOADS_ENTRIES));
    fprintf(stderr,
            "  Dequeue iterations where queue was empty: %zu (%zu "
            "dequeue calls total)\n",
            stat_dequeue_queue_empty_iterations, stat_num_dequeues);
    fprintf(stderr, "  ⤷ %.2f iterations/dequeue\n",
            static_cast<double>(stat_dequeue_queue_empty_iterations) /
                    static_cast<double>(stat_num_dequeues));
    fprintf(stderr, "\n");

#endif
}

#if PROCESS_CHANNEL_STATS
void ProcessChannel::DumpWaitStats(int mpi_rank, FILE* f) {
    for (auto pair : pending_process_recv_channel_snapshots) {
        fprintf(f, "%lu, %lu, %u, %p\n", pair.first.tv_sec, pair.first.tv_nsec,
                pair.second, this);
    }
}
#endif

// This is called after construction in PureProcess::BindToProcessChannel if
// using the runtime send buffer. Otherwise it's assumed that the user buffer is
// used.
void ProcessChannel::AllocateSendRTBuffer() {
    send_rt_buf_ = std::malloc(num_payload_bytes_);
    if (send_rt_buf_ == nullptr) {
        throw std::bad_alloc();
    }
}

void ProcessChannel::BindSendChannel(SendChannel* const sc) {
    // make sure this is called just once because the SendChannel dictates
    // buffer sizes, etc,
    // and this ProcessChannel was already set up in the constructor.
    assert(send_channel_ == nullptr);
    send_channel_     = sc;
    send_pure_thread_ = sc->GetPureThread();
    // note: it's possible that send_rt_buf is nullptr if using_rt_send_buf_hint
    // in
    // PureProcess was false.
    send_channel_->SetSendChannelBuf(send_rt_buf_); // this remains true
                                                    // throughout the lifetime
                                                    // of this ProcessChannel
    send_channel_->SetInitializedState();
}

void ProcessChannel::BindRecvChannel(RecvChannel* const rc) {
    assert(recv_channel_ == nullptr);
    recv_channel_ = rc;
    assert(recv_channel_->GetRawRecvChannelBuf() == nullptr);

    // initialize the receiver buffer if receiver initializtion function was
    // set
    // note: should probably do this for the sender buffer now.
    if (recv_channel_->HasBufferInitializerFunc()) {
        // initialize just the first buffer slot. the other buffer slots
        // will be
        // written by send.
        sentinel("For ProcessChannel, buffer initializer functions are not "
                 "implemented. If you want to do this, "
                 "you'll have to somehow initialize the first buffer entry "
                 "(which hasn't been allocated yet).");
    }

    recv_pure_thread_ = rc->GetPureThread();
    recv_channel_->SetInitializedState();

    // note: not implementing condition frame for initialization as nothing
    // happens here that the sender depends on (and
    // vice versa). This could be problematic, but I can't think of anything
    // right now.
}

void ProcessChannel::WaitForInitialization() const { return; }

// uses runtime buffer
void ProcessChannel::Enqueue(const size_t count_to_send) {
    EnqueueImpl(send_rt_buf_, count_to_send);
}

// uses user buffer
void ProcessChannel::EnqueueUserBuf(const buffer_t user_buf,
                                    const size_t   count_to_send) {

#if DEBUG_CHECK
    if (send_rt_buf_ != nullptr) {
        sentinel("EnqueueUserBuf called for ProcessChannelV3 but send_rt_buf "
                 "(the runtime sender buf) was allocated, "
                 "which means both a user send buffer and runtime send "
                 "buffer were allocated and memory is likely wasted / "
                 "over-allocated. To fix, pass using_rt_send_buf_hint "
                 "when initializing the SendChannel in the user program.");
    }
#endif

    EnqueueImpl(user_buf, count_to_send);
}

// Called only by sender thread
void ProcessChannel::EnqueueImpl(const buffer_t sender_buf,
                                 const size_t   count_to_send) {

    // step 0: error checking
    CheckInvariants();

    // step 1: copy the send buffer into the next buffered payloads slot.
    // Note that the next slot is managed by the buffered payloads data
    // structure.
    EnqueueSpinWhileFull(sender_buf, count_to_send);

#if PROCESS_CHANNEL_STATS
    const size_t s = buffered_payloads_.sizeGuess();
    if (s > stat_buffered_payloads_max_sz) {
        stat_buffered_payloads_max_sz = s;
    }
    ++stat_num_enqueues;
#endif
}

void ProcessChannel::EnqueueSpinWhileFull(const buffer_t sender_buf,
                                          const size_t   count_to_send) {
    // TODO: make send_channel_->GetDatatypeBytes() a function argument or just
    // cache it in the process channel itself.

    // spin until the queue (buffered_payloads_) has room
    while (!buffered_payloads_.writeAndCopyBuf(
            sender_buf, false, count_to_send,
            count_to_send * send_channel_->GetDatatypeBytes()
#if COLLECT_THREAD_TIMELINE_DETAIL
                    ,
            send_pure_thread_->pc_memcpy_timer
#endif
            )) {

#if PCV3_OPTION_ACTIVE_PAUSE
        x86_pause_instruction();
#endif

#if PROCESS_CHANNEL_STATS
        ++stat_enqueue_queue_full_iterations;
#endif
        continue;
    }
}

void ProcessChannel::EnqueueWait() { return; }

// TODO: reinline this or add a way to not do attribute noinline for release
// mode
void* ProcessChannel::DequeueSpinWhileEmpty(void* sbp) {
    while (sbp == nullptr) {
#if PCV3_OPTION_ACTIVE_PAUSE
        x86_pause_instruction();
#endif
#if PROCESS_CHANNEL_STATS
        ++stat_dequeue_queue_empty_iterations;
#endif
        sbp = buffered_payloads_.frontPtr();
    }
    return sbp;
}

void ProcessChannel::Dequeue() {

    // step -1: error checking
    CheckInvariants();

    // step 0: return buffer from previous dequeue, as it is now done being
    // used. the only time we don't want to return it is on the first use, in
    // which case there is nothing to return. On the first use the buffer will
    // be nullptr.
    if (recv_channel_->GetRawRecvChannelBuf() != nullptr) {
        buffered_payloads_.popFront();
    }

    // step 2: get the pointer to the buffer where the message resides from
    // buffered payloads.

    // The data structure returned from frontPtr has the following form:
    //
    //    [ payload_count    |  _pad_to_next_cacheline    |  void* buf ]
    //            ↑                       ↑                      ↑
    //         uint32_t        count_cacheline_pad_bytes bytes_per_payload_buf
    //   .................... or less if the enqueue gave a smaller amount
    void* sbp = buffered_payloads_.frontPtr();
    sbp       = DequeueSpinWhileEmpty(sbp);

// correct for counting one too many as on the last iteration
// dequeue_queue_empty_iteration will have succeeded
#if PROCESS_CHANNEL_STATS
    --stat_dequeue_queue_empty_iterations;
    ++stat_num_dequeues;
#endif

    // step 4: store this count and pointer in the RecvChannel
    auto payload_count = static_cast<uint32_t*>(sbp)[0];
    recv_channel_->SetReceivedCount(payload_count);

    buffer_t buf_ptr = static_cast<uint8_t*>(sbp) + buf_ptr_offset;
    recv_channel_->SetRecvChannelBuf(buf_ptr);
}

void ProcessChannel::DequeueWait() { return; }

// invarients will only be true after ProcessChannel is initialized
void ProcessChannel::CheckInvariants() const {
#if PROCESS_CHANNEL_DEBUG
    if (send_channel_ != nullptr && recv_channel_ != nullptr) {
        assert(send_channel_->NumPayloadBytes() ==
               recv_channel_->NumPayloadBytes());
    }
#endif
}

#ifdef DEBUG_CHECK
char* ProcessChannel::GetDescription() {
    CheckInvariants();
    sprintf(description_, "\tr%d -> r%d", send_channel_->GetSenderPureRank(),
            recv_channel_->GetDestPureRank());
    return description_;
};
#endif

} // namespace process_channel_v3
