// Author: James Psota
// File:   direct_channel_batcher.h 

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

#ifndef DIRECT_CHANNEL_BATCHER_H
#define DIRECT_CHANNEL_BATCHER_H

#include "mpi.h"
#include "pure/common/pure_rt_enums.h"
#include "pure/support/helpers.h"
#include <atomic>
#include <optional>

using std::atomic;
using std::optional;

#define ENABLE_DCB_TIMEOUT 0

// hack-- turn these into constructor params or something
static const int   send_bytes_threshold            = DCB_SEND_BYTES_THRESHOLD;
static const float send_bytes_buf_factor           = 1.75;
static const int   send_occupancy_cycles_threshold = 100000000;
// TODO: verify this on the sender side
static const int max_messages_per_batch = DCB_MAX_MESSAGES_PER_BATCH;

#define BATCHER_HEADER_NONCE 1111111111

struct BatcherHeader {
#if DEBUG_CHECK
    const int start_word = BATCHER_HEADER_NONCE;
#endif
    const int          payload_bytes;
    const int          sender_pure_rank;
    const int          receiver_pure_rank;
    const MPI_Datatype datatype;
    const int          tag;

    BatcherHeader(int pb, int s, int r, int d, int t)
        : payload_bytes(pb), sender_pure_rank(s), receiver_pure_rank(r),
          datatype(d), tag(t) {}
};

struct alignas(CACHE_LINE_BYTES) DestThreadMessageCompletionTracker {
    alignas(CACHE_LINE_BYTES) bool msg_idx_for_me_consumed
            [max_messages_per_batch];
    char _pad[CACHE_LINE_BYTES - (CACHE_LINE_BYTES % max_messages_per_batch)];

    DestThreadMessageCompletionTracker() {
        // fprintf(stderr,
        //         "max_messages_per_batch %d  sizeof compltion tracker %d this:
        //         "
        //         "%p  msg_idx_for_me_consumed: %p  "
        //         "alignof(DestThreadMessageCompletionTracker): %d\n",
        //         max_messages_per_batch,
        //         sizeof(DestThreadMessageCompletionTracker), this,
        //         msg_idx_for_me_consumed,
        //         alignof(DestThreadMessageCompletionTracker));

        // For some reason this isn't working on cori. punting for now.
        // PureRT::assert_cacheline_aligned(this);
        // PureRT::assert_cacheline_aligned(msg_idx_for_me_consumed);
        MarkAllNotComplete();
        static_assert(sizeof(DestThreadMessageCompletionTracker) %
                              CACHE_LINE_BYTES ==
                      0);
    }
    ~DestThreadMessageCompletionTracker() = default;

    void MarkAllNotComplete() {
        memset(msg_idx_for_me_consumed, 0,
               max_messages_per_batch * sizeof(bool));
    }

    bool IsMsgNumCompleted(int msg_num) const {
        assert(msg_num >= 0);
        assert(msg_num < max_messages_per_batch);
        return msg_idx_for_me_consumed[msg_num];
    }

    // msg_num is zero indexed
    void MarkMsgNumComplete(int msg_num) {
        assert(msg_num >= 0);
        assert(msg_num < max_messages_per_batch);
        msg_idx_for_me_consumed[msg_num] = true;
    }
};

class DirectChannelBatcher {
  public:
    DirectChannelBatcher(int                  mate_mpi_rank_arg,
                         PureRT::EndpointType endpoint_type_arg,
                         int threads_per_process, bool is_first_batcher);
    ~DirectChannelBatcher();
    void Finalize();
    void CancelOutstandingRequest();

    bool EnqueueSingleMsg(void* msg_payload, int payload_bytes,
                          int sender_pure_rank, int receiver_pure_rank,
                          MPI_Datatype datatype, int tag, bool&);
    void ResetEnqueue(bool calling_from_constructor);

    void DoDequeueMpiIrecv();
    bool DequeueWaitAndTryToConsumeSingleMessage(
            void* const dest_buf, int payload_max_bytes, int sender_pure_rank,
            int receiver_pure_rank, MPI_Datatype datatype, int tag,
            int thread_num_in_proc, bool& became_send_leader);

    // management functions
    void AllowDequeueEntry();
    void DisallowDequeueEntry();
    void WaitUntilNoActiveDequeueThreads() const;
    void ResetDequeue();
    void WaitUntilDequeueEntryAllowed() const;

  private:
    DestThreadMessageCompletionTracker* thread_completion_trackers;
    void*                               batch_buf;

    const PureRT::EndpointType endpoint_type;
    const int                  mate_mpi_rank;
    const int                  num_threads;
    const int batch_tag = PureRT::PURE_DIRECT_CHAN_BATCHER_TAG; // special tag
    const int thread_completion_trackers_total_bytes;
    int       send_bytes_max;

    // TODO: cache alignemnt and reordering

    // need to reset these on each use
    // concurrent fields
#if ENABLE_DCB_TIMEOUT
    alignas(CACHE_LINE_BYTES)
            // used by senders only -- no false sharing
            atomic<unsigned long> cycle_of_first_message_enqueued;
#endif

    // The number of bytes added to the batch_buf, to be sent out using MPI.
    // And, importantly, it is the main synchronization mechanism to
    // indicate that a send is in progress and is used to indicate that the
    // message has been sent.
    // for receiver, bytes_promised means bytes_in_message
    // we keep these on the same line as they are used together often, and it
    // allows us to reset them simultaneously.
    alignas(CACHE_LINE_BYTES) atomic<int> bytes_promised;
    atomic<int> bytes_committed_or_consumed;

    /// only for recv
    // used for both send and recv, depending on direction. it's the
    // MPI_Request associated with the send and receive. It is actually
    // used as a synchronization mechanism to determine which thread
    // does the MPI_Isend.
    alignas(CACHE_LINE_BYTES) atomic<MPI_Request> outstanding_req;

    // used to elect the thread that does the wait
    alignas(CACHE_LINE_BYTES) atomic<int> wait_leader;

    alignas(CACHE_LINE_BYTES) atomic<int> active_threads;
    atomic<bool> entry_allowed;

    //////////////////////
    void DoEnqueueMPISend(int) const;
    bool ReadyToSend() const;
    bool SendDueToTimeout(int) const;
    bool SendIfPossible();
    ////////
    bool DequeueWaitInProgress() const;
    bool AllDequeueMsgsConsumed(int&) const;
    DestThreadMessageCompletionTracker&
         GetCompletionTracker(int thread_num_in_proc);
    void ResetCommLeader();

    // Turnstyle functions
    void ThreadEnteredDequeue();
    void ThreadExitedDequeue();
    bool DequeueEntryAllowed() const;
    bool DequeueEntryNotAllowed() const;

    //////////////////////
    static void PrintBatchedMessage(DirectChannelBatcher const* const, void*,
                                    int, bool);
};

#endif