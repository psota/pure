// Author: James Psota
// File:   process_channel_v2.cpp 

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

// Note: we include process_channel.h, not process_channel_v2.h so
// the proper feature and member options are defined as appropriate for
// the user-specified process channel version. This is a bit circular,
// but it seems to work.
#include "pure/transport/process_channel.h"
#include "pure/transport/recv_channel.h"
#include "pure/transport/send_channel.h"

namespace process_channel_v2 {

// called either by sender thread or receiver thread via PureProcess. only gets
// called once per sender/receiver pair.
ProcessChannel::ProcessChannel(int num_payload_bytes_arg)
    : num_payload_bytes_(num_payload_bytes_arg), send_rt_buf_(nullptr),
      send_channel_(nullptr), recv_channel_(nullptr),
      buffered_payloads_(PROCESS_CHANNEL_BUFFERED_MSG_SIZE)
#if PCV2_OPTION_USE_FREELIST
      ,
      free_buffers_(PROCESS_CHANNEL_BUFFERED_MSG_SIZE)
#endif
{
}

// called once by PureProcess inside PureProcess destructor
ProcessChannel::~ProcessChannel() {
    if (send_rt_buf_ != nullptr) {
        std::free(send_rt_buf_);
    }
    if (recv_channel_->GetRawRecvChannelBuf()) {
        std::free(recv_channel_->GetRawRecvChannelBuf());
    }

    size_t sz0 = buffered_payloads_.sizeGuess();
    check(sz0 == 0,
          "Upon finishing using this ProcessChannel (v2) the "
          "buffered_payloaded SPSC queue should have zero entries in "
          "it, assuming the "
          "number of receives exactly matched the number of sends. "
          "However, there are %zu entries on it still.",
          buffered_payloads_.sizeGuess());

#if PCV2_OPTION_USE_FREELIST
    // deallocate memory still on the free buffers list
    const size_t free_buffers_size_at_dcon = free_buffers_.sizeGuess();
    buffer_t     buf_entry_to_free;
    while (free_buffers_.read(buf_entry_to_free)) {
        std::free(buf_entry_to_free);
    }
    sz0 = free_buffers_.sizeGuess();
    check(sz0 == 0,
          "Upon finishing using this ProcessChannel (v2) the "
          "free_buffers_ SPSC queue "
          "should have zero entries in it. However, there are %zu "
          "entries on it still.",
          sz0);
#endif

#if PROCESS_CHANNEL_STATS
    fprintf(stderr,
            "ProcessChannel v2 Deconstructor Stats *****************\n");
    fprintf(stderr, "  buffered messages size (max in flight): %zu\n",
            PROCESS_CHANNEL_BUFFERED_MSG_SIZE);
    fprintf(stderr,
            "  Enqueue iterations where queue was full: %zu (%zu "
            "enqueue calls total)\n",
            stat_enqueue_queue_full_iterations, stat_num_enqueues);
    fprintf(stderr, "  ~Max buffered payloads queue size: %zu\n",
            stat_buffered_payloads_max_sz);
    fprintf(stderr,
            "  Dequeue iterations where queue was empty: %zu (%zu "
            "dequeue calls total)\n",
            stat_dequeue_queue_empty_iterations, stat_num_dequeues);
    fprintf(stderr, "  free bufs gotten:\t%zu\n", stat_free_bufs_gotten);
    fprintf(stderr, "   ⤷ buffers malloced:\t%zu (%.2f%%)\n",
            stat_buffers_malloced,
            percentage<int>(stat_buffers_malloced, stat_free_bufs_gotten));
    fprintf(stderr, "  free bufs returned:\t%zu\n", stat_free_bufs_returned);
#if PCV2_OPTION_USE_FREELIST
    fprintf(stderr,
            "  number of buffers on free list at deconstruction:\t%zu\n",
            free_buffers_size_at_dcon);
    fprintf(stderr, "  ~Max free buffers size: %zu\n",
            stat_free_buffers_max_sz);
#endif

    fprintf(stderr, "\n");
#endif
}

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
    send_channel_ = sc;
    send_channel_->SetSendChannelBuf(send_rt_buf_); // this remains true
                                                    // throughout the lifetime
                                                    // of this ProcessChannel
    send_channel_->SetInitializedState();
}

void ProcessChannel::BindRecvChannel(RecvChannel* const rc) {
    assert(recv_channel_ == nullptr);
    recv_channel_ = rc;
    assert(recv_channel_->GetRawRecvChannelBuf() == nullptr);

    // initialize the receiver buffer if receiver initializtion function was set
    // note: should probably do this for the sender buffer now.
    if (recv_channel_->HasBufferInitializerFunc()) {
        // initialize just the first buffer slot. the other buffer slots will be
        // written by send.
        sentinel("For ProcessChannel, buffer initializer functions are not "
                 "implemented. If you want to do this, "
                 "you'll have to somehow initialize the first buffer entry "
                 "(which hasn't been allocated yet).");
    }
    recv_channel_->SetInitializedState();

    // note: not implementing condition frame for initialization as nothing
    // happens here that the sender depends on (and
    // vice versa). This could be problematic, but I can't think of anything
    // right now.
}

void ProcessChannel::WaitForInitialization() const { return; }

// uses runtime buffer
bool ProcessChannel::Enqueue(const size_t count_to_send) {
    return EnqueueImpl(send_rt_buf_, count_to_send);
}

// uses user buffer
bool ProcessChannel::EnqueueUserBuf(const buffer_t user_buf,
                                    const size_t   count_to_send) {
    return EnqueueImpl(user_buf, count_to_send);
}

// Called only by sender thread
// private
bool ProcessChannel::EnqueueImpl(const buffer_t sender_buf,
                                 const size_t   count_to_send) {

    // step 0: error checking
    CheckInvarients();

    // step 1: get a buffer to copy into
    buffer_t buffered_payload = GetFreeBuf();

    // step 2: copy into fixed sender buffer
    // note that the sender will always place payload into this same buffer
    // throughout the lifetime of this ProcessChannel
    std::memcpy(buffered_payload, sender_buf, num_payload_bytes_);

    // step 3: initialize the entry to go into the buffered_payload queue
    // shared_queue_entry_t entry_to_enqueue;
    // entry_to_enqueue.payload_count = count_to_send;
    // entry_to_enqueue.buf = buffered_payload;

    // step 4: insert it into the buffered_payload queue

    // using arguments for shared_queue_entry_t constructor
    while (!buffered_payloads_.write(count_to_send, buffered_payload)) {
        // spin until the queue (buffered_payloads_) has room

#if PCV2_OPTION_ACTIVE_PAUSE
        x86_pause_instruction();
#endif
#if PROCESS_CHANNEL_STATS
        ++stat_enqueue_queue_full_iterations;
#endif
        continue;
    }

#if PROCESS_CHANNEL_STATS
    const size_t s = buffered_payloads_.sizeGuess();
    if (s > stat_buffered_payloads_max_sz) {
        stat_buffered_payloads_max_sz = s;
    }
    ++stat_num_enqueues;
#endif
    // step 4: return true, as the message was sent;
    // consider removing this programming model
    // detail in the future
    // in line with semantics of BundleChannel::Enqueue
    const bool msg_sent = true;
    return msg_sent;
}

bool ProcessChannel::Dequeue() {

    // step -1: error checking
    CheckInvarients();

    // step 0: return buffer from previous dequeue, as it is now done being
    // used
    // the only time we don't want to return it is on the first use, in
    // which
    // case there is nothing to return. On the first use the buffer will be
    // nullptr.
    if (recv_channel_->GetRawRecvChannelBuf() != nullptr) {
        ReturnFreeBuf(recv_channel_->GetRawRecvChannelBuf());
    }

    // step 1: get value from buffered_payloads_
    // TODO: spin until we get a value
    // buffer_t* ptr_to_receive_buffer_t = nullptr;
    shared_queue_entry_t* ptr_to_receive_entry_t =
            buffered_payloads_.frontPtr();

    while (ptr_to_receive_entry_t == nullptr) {
#if PCV2_OPTION_ACTIVE_PAUSE
        x86_pause_instruction();
#endif
#if PROCESS_CHANNEL_STATS
        ++stat_dequeue_queue_empty_iterations;
#endif
        ptr_to_receive_entry_t = buffered_payloads_.frontPtr();
    }

// TODO: this is weird. this was in there before as a second grab. I think it
// worked because it was just getting the frontptr again, which was the same
// value and only executed the loop once because the termination condition was
// already acheived.

//         do {
//             ptr_to_receive_entry_t = buffered_payloads_.frontPtr();
// #if PROCESS_CHANNEL_STATS
//             ++stat_dequeue_queue_empty_iterations;
// #endif
//         } while (ptr_to_receive_entry_t ==
//                  nullptr); // spin until we get a value

// correct for counting one too many as on the last iteration
// dequeue_queue_empty_iteration will have succeeded
#if PROCESS_CHANNEL_STATS
    --stat_dequeue_queue_empty_iterations;
    ++stat_num_dequeues;
#endif

// step 2: update the relevant pointer in RecvChannel with this buffer pointer
#if PCV2_OPTION_USE_PADDED_BUF_PTR
    PureRT::assert_cacheline_aligned(ptr_to_receive_entry_t);
#endif
    recv_channel_->SetReceivedCount(ptr_to_receive_entry_t->payload_count);
    recv_channel_->SetRecvChannelBuf(ptr_to_receive_entry_t->buf);

    // step 3: pop off the value from buffered_payloads_, as it's now
    // "consumed"
    // by the receiver
    buffered_payloads_.popFront();

    // step 4: return true, as the message was sent;
    // consider removing this programming model
    // detail in the future
    // in line with semantics of BundleChannel::Dequeue
    const bool msg_received = true;
    return msg_received;
}

void ProcessChannel::Wait() const {
    // Do nothing
}

buffer_t ProcessChannel::GetFreeBuf() {
    buffer_t new_buf;
#if PCV2_OPTION_USE_FREELIST
    // emply free buffer list approach
    if (free_buffers_.read(new_buf)) {
        // we found a free buffer, so no need to allocate a new one. yay.
    } else {
        // no free items available, so create a new one. :(
        // TODO: may be faster to allocated an aligned buffer here.
        new_buf = static_cast<buffer_t>(std::malloc(num_payload_bytes_));
#if PROCESS_CHANNEL_STATS
        ++stat_buffers_malloced;
#endif
    }

#else
    // just allocate a buffer each time
    new_buf = static_cast<buffer_t>(std::malloc(num_payload_bytes_));
#if PROCESS_CHANNEL_STATS
    ++stat_buffers_malloced;
#endif
#endif

#if PROCESS_CHANNEL_STATS
    ++stat_free_bufs_gotten;
#endif

    return new_buf;
}

void ProcessChannel::ReturnFreeBuf(buffer_t buf_to_recycle) {
#if PCV2_OPTION_USE_FREELIST

    /* original v21 and v22 version:
       exhibits deadlock when free_buffers_ gets full
        while (!free_buffers_.write(buf_to_recycle)) {
            // spin until the queue has room
            continue;
        }
    */
    if (!free_buffers_.write(buf_to_recycle)) {
        // free buffers is full, so just free this buffer
        // TODO: consider alternative where we spin to wait until there's
        // room
        // both in GetFreeBuf and here in ReturnFreeBuf
        std::free(buf_to_recycle);
    }

#else
    std::free(buf_to_recycle);
#endif

#if PROCESS_CHANNEL_STATS
    ++stat_free_bufs_returned;
#if PCV2_OPTION_USE_FREELIST
    const size_t s = free_buffers_.sizeGuess();
    if (s > stat_free_buffers_max_sz) {
        stat_free_buffers_max_sz = s;
    }
#endif
#endif
}

// invarients will only be true after ProcessChannel is initialized
void ProcessChannel::CheckInvarients() const {
#if PROCESS_CHANNEL_DEBUG
    if (send_channel_ != nullptr && recv_channel_ != nullptr) {
        assert(send_channel_->NumPayloadBytes() ==
               recv_channel_->NumPayloadBytes());
    }
#endif
}

#ifdef DEBUG_CHECK
char* ProcessChannel::GetDescription() {
    sprintf(description_, "\tr%d -> r%d", send_channel_->GetSenderPureRank(),
            recv_channel_->GetDestPureRank());
    return description_;
};
#endif

} // namespace process_channel_v2
