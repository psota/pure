// Author: James Psota
// File:   process_channel.cpp 

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

/*
  This version of ProcessChannel is in experimental because of a couple of unresolved challenges in trying to use runtime
  buffers. 
  1) The buffer changed on each use. this is not ideal from a programming model standpoint.
  2) Initializing two buffers when they both point to the same place is hard or impossible. This
     came up with jacobi's matrix initialization.
  3) Buffer ownership and valid lifetime is likely to be a problem in more complicated applications. 

  So, keeping this implementation (and the corresponding header file in the include dir) in experimental for now. Some 
  ideas from it may arise again.
*/


#include "pure/transport/process_channel.h"
#include "pure/transport/recv_channel.h"

ProcessChannel::ProcessChannel(size_t buf_bytes_, size_t num_buffered_entries_)
    : /* first index to write to is already set to zero */
      sender_next_write_idx(0),
      /* start off at -1 so when it gets incremented on first use by
                GetRecvChannelBuf(), it goes to zero */
      receiver_active_read_idx(receiver_active_read_idx_init),
      /* starts off at 1 so when it gets decremented by GetRecvBuf on the first time through, it goes down to zero,
         which is where we need it. This saves a branch on every iteration. There's a corner case where one element
         will
         be wasted if the sender gets way ahead of the receiver, but that's probably unlikely */
      num_unread(1), num_buffered_entries(num_buffered_entries_), send_channel(nullptr), recv_channel(nullptr)
#if DEBUG_CHECK
      ,
      sender_state(ProcessChannelSenderState::invalid), receiver_state(ProcessChannelReceiverState::invalid)
#endif
{

    buffers = new char*[num_buffered_entries];
    for (auto i = 0; i < num_buffered_entries; ++i) {
        buffers[i] = new char[buf_bytes_];
    }
}

ProcessChannel::~ProcessChannel() {
    for (auto i = 0; i < num_buffered_entries; ++i) {
        delete[] (buffers[i]);
    }
    delete[](buffers);
    fprintf(stderr, KYEL "\nWARNING: upon deallocation of ProcessChannel, num_unread = %d out of %d\n" KRESET, num_unread,
            num_buffered_entries);
}

void ProcessChannel::BindSendChannel(SendChannel* const sc) {
    // make sure this is called just once because the SendChannel dictates buffer sizes, etc,
    // and this ProcessChannel was already set up in the constructor.
    assert(send_channel == nullptr);
    send_channel = sc;
}

void ProcessChannel::BindRecvChannel(RecvChannel* const rc) {
    // make sure this is called just once because the RecvChannel dictates buffer sizes, etc,
    // and this ProcessChannel was already set up in the constructor.
    assert(recv_channel == nullptr); // make sure this is called just once
    recv_channel = rc;

    if (rc->HasBufferInitializerFunc()) {
        // initialize just the first buffer slot. the other buffer slots will be written by send.
        rc->InitBuffer(buffers[0]);
    }

    sender_state = ProcessChannelSenderState::initialized;
    receiver_state = ProcessChannelReceiverState::initialized;
    initializer_done_initializing_cf.MarkReadyAndNotifyOne();
}

void ProcessChannel::WaitForInitialization() {
    initializer_done_initializing_cf.WaitForReady();
} // function WaitForInitialization

void* ProcessChannel::GetNewSendChannelBuf() {

    VerifySenderState(ProcessChannelSenderState::initialized, ProcessChannelSenderState::enqueued_not_writable);

#if ERROR_IF_BUFFER_FULL
    {
        std::lock_guard<std::mutex> index_lock(index_mutex);

        // TODO: consider changing this to an atomic load
        if (num_unread >= num_buffered_entries) {
            fprintf(stderr, "ERROR: ProcessChannel buffer slots full (at capacity of %d). Increase "
                            "num_buffered_entries.\n",
                    num_unread);
            if (receiver_active_read_idx == receiver_active_read_idx_init) {
                fprintf(stderr, "WARNING: no receive calls have been made (as receiver_active_read_idx=%d) so one "
                                "slot of storage in the buffer was wasted. Try to disallow this from happening.\n",
                        receiver_active_read_idx);
            }
            exit(EXIT_FAILURE);
        }
    } // ends lock
#else
    {
        std::unique_lock<std::mutex> index_lock(index_mutex);
        buffers_full_cv.wait(index_lock, [this]() { return this->num_unread < num_buffered_entries; });
    }
#endif

    IncrementSenderState();
    return buffers[sender_next_write_idx];
}

void ProcessChannel::WaitUntilBufWritable() {
    VerifySenderState(ProcessChannelSenderState::have_buffer_waiting_until_writable);
    IncrementSenderState();
}

bool ProcessChannel::BufferWritable() const {
    VerifySenderState(ProcessChannelSenderState::writable);
    return true;
}

bool ProcessChannel::Enqueue() {

    VerifySenderState(ProcessChannelSenderState::writable);

    {
        std::lock_guard<std::mutex> index_lock(index_mutex);
        ++num_unread;
        // ts_log_info("SENDER: num_unread = %d\n", num_unread);
    }
    payload_available_cv.notify_one();
    circular_increment(num_buffered_entries, sender_next_write_idx);
    IncrementSenderState(); // now enqueued_not_writable
    return true;            // by this point the message is there and readable_writable, so it's considered "sent"
}

void* ProcessChannel::GetNewRecvChannelBuf() {

    VerifyReceiverState(ProcessChannelReceiverState::initialized, ProcessChannelReceiverState::readable_writable);
    {
        std::lock_guard<std::mutex> index_lock(index_mutex);
        --num_unread;
    }
#if !ERROR_IF_BUFFER_FULL
    buffers_full_cv.notify_one();
#endif
    circular_increment(num_buffered_entries, receiver_active_read_idx);
    IncrementReceiverState();
    return buffers[receiver_active_read_idx];
}

void ProcessChannel::Dequeue() {
    VerifyReceiverState(ProcessChannelReceiverState::have_unusable_buffer);
    IncrementReceiverState(); // now waiting_for_readable_writable
}

void ProcessChannel::ReceiverWait() { // known as "Wait()" by user

    VerifyReceiverState(ProcessChannelReceiverState::waiting_for_readable_writable);
    {
        std::unique_lock<std::mutex> index_lock(index_mutex);
        // ts_log_info("RECEIVER: num_unread = %d\n", num_unread);
        check(num_unread >= 0, "num_unread (%d) can't be less than zero", num_unread);
        payload_available_cv.wait(index_lock, [this]() { return this->num_unread > 0; });
        check(num_unread > 0, "num_unread (%d) must be greater than zero now", num_unread);
    }
    IncrementReceiverState(); // now readable_writable
    VerifyReceiverState(ProcessChannelReceiverState::readable_writable);
}

bool ProcessChannel::BufferValidForReadWrite() {
    VerifyReceiverState(ProcessChannelReceiverState::readable_writable);
    {
        std::lock_guard<std::mutex> index_lock(index_mutex);
        return num_unread > 0;
    }
}

//// State Management //////////////////////////////////////
#if DEBUG_CHECK
ProcessChannelSenderState ProcessChannel::NextSenderState() {

    ProcessChannelSenderState new_state;
    switch (sender_state) {
    case ProcessChannelSenderState::initialized:
        new_state = ProcessChannelSenderState::have_buffer_waiting_until_writable;
        break;
    case ProcessChannelSenderState::have_buffer_waiting_until_writable:
        new_state = ProcessChannelSenderState::writable;
        break;
    case ProcessChannelSenderState::writable:
        new_state = ProcessChannelSenderState::enqueued_not_writable;
        break;
    case ProcessChannelSenderState::enqueued_not_writable:
        new_state = ProcessChannelSenderState::have_buffer_waiting_until_writable;
        break;
    default:
        sentinel("Invalid state %d", sender_state);
    }
    return new_state;
}

ProcessChannelReceiverState ProcessChannel::NextReceiverState() {

    ProcessChannelReceiverState new_state;
    switch (receiver_state) {
    case ProcessChannelReceiverState::initialized:
        new_state = ProcessChannelReceiverState::have_unusable_buffer;
        break;
    case ProcessChannelReceiverState::have_unusable_buffer:
        new_state = ProcessChannelReceiverState::waiting_for_readable_writable;
        break;
    case ProcessChannelReceiverState::waiting_for_readable_writable:
        new_state = ProcessChannelReceiverState::readable_writable;
        break;
    case ProcessChannelReceiverState::readable_writable:
        new_state = ProcessChannelReceiverState::have_unusable_buffer;
        break;
    default:
        sentinel("Invalid state %d", receiver_state);
    }
    return new_state;
}
#endif

void ProcessChannel::IncrementSenderState() {
#if DEBUG_CHECK
    sender_state = NextSenderState();
#endif
}
void ProcessChannel::IncrementReceiverState() {
#if DEBUG_CHECK
    receiver_state = NextReceiverState();
#endif
}

// WARNING: this is duplicated below. Refactor all of this.
void ProcessChannel::VerifySenderState(ProcessChannelSenderState s1,
                                       ProcessChannelSenderState s2) const {
#if DEBUG_CHECK
    if (s2 == ProcessChannelSenderState::invalid) {
        // single valid state case
        if (sender_state != s1) {
            sentinel("Expected sender state to be %d, but is %d", s1, sender_state);
        }
    } else {
        // two valid state case
        if (sender_state != s1 && sender_state != s2) {
            sentinel("Expected sender state to be either %d or %d, but is %d", s1, s2, sender_state);
        }
    }
#endif
}
void ProcessChannel::VerifyReceiverState(ProcessChannelReceiverState s1,
                                         ProcessChannelReceiverState s2) const {
#if DEBUG_CHECK
    if (s2 == ProcessChannelReceiverState::invalid) {
        // single valid state case
        if (receiver_state != s1) {
            sentinel("Expected receiver state to be %d, but is %d", s1, receiver_state);
        }
    } else {
        // two valid state case
        if (receiver_state != s1 && receiver_state != s2) {
            sentinel("Expected receiver state to be either %d or %d, but is %d", s1, s2, receiver_state);
        }
    }
#endif
}
// WARNING: this is duplicated above. Refactor all of this.
