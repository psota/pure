// Author: James Psota
// File:   process_channel_v4.h 

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

#ifndef PURE_TRANSPORT_PROCESS_CHANNEL_V4_H
#define PURE_TRANSPORT_PROCESS_CHANNEL_V4_H

#pragma once

#include "pure/common/pure_pipeline.h"
#include "pure/support/benchmark_timer.h"
#include "pure/support/typed_producer_consumer_queue.h"
#include "pure/support/untyped_producer_consumer_queue.h"
#include "pure/support/zed_debug.h"
#include "pure/transport/pending_channel_manager.h"
#include <atomic>
#include <boost/circular_buffer.hpp>
#include <optional>
#include <variant>

// TODO: turn this back on generally after working through TSAN issues
#define PROCESS_CHANNEL_DEBUG (DEBUG_CHECK)
#define PROCESS_CHANNEL_STATS (PRINT_PROCESS_CHANNEL_STATS)
#define PROCESS_CHANNEL_4_BUFFERED_PAYLOADS_ENTRIES \
    (PROCESS_CHANNEL_BUFFERED_MSG_SIZE)

#define PCV4_APPR_2_DEBUG 0

// forward declarations
class SendChannel;
class RecvChannel;
class PureThread;

using namespace PureRT;
using WaitStatContainer = std::vector<std::pair<std::string, uint64_t>>;

/*
 * Scheduling Strategy __________________________
 *
 * PCV4_OPTION_EXECUTE_CONTINUATION_SR_COLLAB
 *   Always collaborate on each if the continuation is collaborative just after
 * completing an enqueue. In the future, we could add an option to collaborate
 * every N tries.
 *
 * PCV4_OPTION_EXECUTE_CONTINUATION_WORK_STEAL
 *   A sender may steal work from a collaborative continuation running within
 * the same process (i.e., PureProcess) when waiting for other things on the
 * critical path to happen. Examples of critical path items include: 1) a sender
 * is waiting for space in its queue to send a message (ProcessChannel) 2) a
 * receiver is waiting for an incoming message to arrive (ProcessChannel and
 * DirectMPIChannel) 3) at the end of the program when some threads are done
 * early 4) ?? others? Look at thread timeline and profile for ideas on where
 * spinning is happening
 */

// namespace process_channel_v4 {

// struct definition for non-buffered channels (channels that don't use
// buffered_payloads)
struct DequeueReq {
    buffer_t __restrict dest;
    //    int      count;

    // default constructor -- garbage data
    DequeueReq() {}

    // DequeueReq(buffer_t dest, int count)
    //     : dest(dest), count(count) {}
    DequeueReq(buffer_t dest) : dest(dest) {}
};

// we must align this as CACHE_LINE_BYTES because this is an aggregate class
// which has a UntypedProducerConsumerQueue which has 64 byte alignment.
class alignas(CACHE_LINE_BYTES) ProcessChannel {

    friend class BundleChannelEndpoint;

  public:
    ProcessChannel(int /*num_payload_bytes*/, bool /* use_buffered_chan */,
                   size_t /* max_buffered_msgs */);
    ~ProcessChannel();

    void AllocateSendRTBuffer();
    void BindSendChannel(SendChannel* /*sc*/);
    void BindRecvChannel(RecvChannel* /*rc*/);
#if PCV4_OPTION_EXECUTE_CONTINUATION
    void        SetDeqContPipeline(PurePipeline&&);
    inline bool HasAnyCollabCont() const {
        return deq_pipeline.HasAnyCollabStage();
    }
#endif

    // void WaitForInitialization() const;
    // void WaitUntilReceiverDoneInitializing() const;

    void Enqueue(const size_t count_to_send);
    void EnqueueUserBuf(const buffer_t __restrict, const size_t count_to_send);
    void EnqueueWait();
    bool EnqueueTest();
    void EnqueueBlocking(const size_t count_to_send);
    void EnqueueBlockingUserBuf(const buffer_t __restrict,
                                const size_t count_to_send);
    size_t BytesToCopy(int count) const;

    void ReceiverDoneInitializing();
    void Dequeue(const buffer_t __restrict);
    const PureContRetArr&
         DequeueWait(bool                 actually_blocking_wait = true,
                     std::optional<bool*> dequeue_succeeded = std::nullopt);
    void DequeueBlocking(const buffer_t __restrict);
#if PCV4_OPTION_OOO_WAIT
    inline bool NoRemainingOutstandingDeqs() {
        // note: it's 1 and not zero because we don't decrement outstanding_deqs
        // until after this dequeue is fully done
        return (outstanding_deqs == 1);
    }
#endif

#if PCV4_OPTION_EXECUTE_CONTINUATION_WORK_STEAL
    bool MaybeHasActiveWorkToSteal() const;

    // https://clang.llvm.org/docs/AttributeReference.html#flatten
    void ExecuteDequeueContinuationUntilDone(PureThread* const)
            __attribute__((flatten));

#if PRINT_PROCESS_CHANNEL_STATS
    // used only for bookkeeping purposes
    inline const PurePipeline* GetDequeuePurePipeline() const {
        return &deq_pipeline;
    }
#endif // PRINT_PROCESS_CHANNEL_STATS

#endif

#if PRINT_PROCESS_CHANNEL_STATS
    void  PrintSenderReceiver() const;
    char* GetDescription();
    int   GetReceiverRank() const;
    int   GetSenderRank() const;
    int   GetReceiverCpu() const;
#endif

  private:
    // SENDER-WRITTEN FIELDS //////////////////////////////////////////////////
    alignas(CACHE_LINE_BYTES) PureThread* send_pure_thread_ = nullptr;
    SendChannel* send_channel_                              = nullptr;
#if PCV4_OPTION_OOO_WAIT
    PendingChannelManager<ProcessChannel>* pending_enqueue_mgr = nullptr;
#endif
    buffer_t  send_buf_          = nullptr;
    size_t    enq_count_to_send_ = -1;
    const int num_payload_bytes_;
    int       cache_line_rounded_bytes_to_copy_ = -1;
    // int       bytes_to_copy_enqueue_;
    int pending_enqueue_index_upon_enqueue = -1;

    bool message_send_done  = false;
    bool send_buf_aligned_  = false;
    bool using_rt_send_buf_ = false;

    // testing -- put deq_reqs on its own line
    alignas(CACHE_LINE_BYTES)
            ProducerConsumerQueue<int>* deq_reqs_payload_counts_;
    //         boost::circular_buffer<size_t> deq_reqs_payload_counts;
    // std::atomic<uint64_t> num_completed_deq_reqs_fulfilled;
    char pad2[CACHE_LINE_BYTES - sizeof(ProducerConsumerQueue<int>*)];

    // RECEIVER-WRITTEN FIELDS /////////////////////////////////////////////////
    alignas(CACHE_LINE_BYTES)
            UntypedProducerConsumerQueue* buffered_payloads_   = nullptr;
    ProducerConsumerQueue<DequeueReq, true>* dequeue_requests_ = nullptr;
    boost::circular_buffer<buffer_t>         buffered_dequeue_dest_bufs;

#if PCV4_OPTION_EXECUTE_CONTINUATION
    PurePipeline deq_pipeline;
#endif
    PureThread*  recv_pure_thread_ = nullptr;
    RecvChannel* recv_channel_     = nullptr;
#if PCV4_OPTION_OOO_WAIT
    PendingChannelManager<ProcessChannel>* pending_dequeue_mgr = nullptr;
#endif

#if PCV4_OPTION_OOO_WAIT
    // this allows us to only call AddPendingQueue on this if we know it's not
    // already there. We could alterantively search the vector but that would be
    // slower.
    unsigned int outstanding_deqs = 0;
#endif
    uint64_t num_completed_dequeues = 0;
    // a wait_consumption is simply a call to wait. this tells Dequeue how many
    // popFronts to do on buffered_payloads_ (effectively freeing up a slot in
    // buffered payloads). That basically means that the payload has been
    // consumed.
    bool ok_to_pop_buffer = false;

#if PCV4_OPTION_OOO_WAIT
    int pending_dequeue_index_upon_dequeue = -1;
#endif

#if PCV4_OPTION_EXECUTE_CONTINUATION
    bool pipeline_initialized       = false;
    bool pipeline_has_collab_stages = false;
#endif

    bool message_receive_done = false;
    bool using_rt_recv_buf_;

    // bad ping pong sharing CACHE LINE OF DEATH, used only with conttinuation
    // execution
    // ////////////////////////////////
    // written by both sender and receiver. at least put it on its own line.
    // todo rmeove
#if PROCESS_CHANNEL_STATS
    alignas(CACHE_LINE_BYTES) uint64_t stat_num_enqueues = 0;
    uint64_t stat_num_dequeues                           = 0;
    uint64_t stat_num_enq_waits                          = 0;
    uint64_t stat_num_deq_waits                          = 0;
    uint64_t stat_enqueue_queue_full_spin_loops          = 0;
    uint64_t stat_deq_wait_queue_empty_spin_loops        = 0;

#if PCV4_OPTION_EXECUTE_CONTINUATION_WORK_STEAL
    // work steals by me
    uint64_t stat_num_work_steal_considerations =
            0; // MaybeWorkStealFromRandomReceiver calls
    uint64_t stat_num_work_steal_attempts =
            0; // ExecuteDequeueContinuationUntilDone calls
#endif

    // Exit path stats for EnqueueWaitTryOOO
    uint64_t stat_ooo_enqueues                   = 0;
    uint64_t stat_enq_wait_ret_mrd_1             = 0;
    uint64_t stat_enq_wait_ret_nbt_2             = 0;
    uint64_t stat_enq_wait_ret_one_pe_3          = 0;
    uint64_t stat_enq_wait_ret_scanned_this_4    = 0;
    uint64_t stat_enq_wait_ret_extra_try_5       = 0;
    uint64_t stat_enq_wait_empty_scan_iterations = 0;

    // Exit path stats for DequeueWaitTryOOO
    uint64_t stat_ooo_dequeues          = 0;
    uint64_t stat_deq_wait_ret_mrd_1    = 0; // message receive done
    uint64_t stat_deq_wait_ret_nbt_2    = 0; // nonblocking try
    uint64_t stat_deq_wait_ret_one_pd_3 = 0; // num pending dequeue = 1
    uint64_t stat_deq_wait_ret_scanned_this_4 =
            0; // scanned pending deqeueues and got this
    uint64_t stat_deq_wait_ret_extra_try_5 =
            0; // after successful pending dequeue, extra try got this
    uint64_t stat_deq_wait_empty_scan_iterations = 0;

    unsigned int stat_buffered_payloads_max_sz = 0;

#include <vector>
    std::vector<std::pair<struct timespec, unsigned int>>
            pending_process_recv_channel_snapshots;
#endif // PROCESS_CHANNEL_STATS

#if PRINT_PROCESS_CHANNEL_STATS
    char description_[64];
#endif

    void EnqueueImpl(const buffer_t buf, const size_t count);
    void EnqueueWaitBlockingThisOnly()
#if PROFILE_MODE
            __attribute__((noinline))
#endif
            ;
    bool EnqueueTryTest();
    bool EnqueueNonblockingTryToSendThis(const buffer_t send_buf,
                                         const size_t   count);

    void FinalizeUnbufferedSend(const buffer_t __restrict source_buf,
                                const size_t enq_count,
                                buffer_t __restrict dest);
    void EnqueueWaitTryOOO();
    void MaybeHelpWithWork();

    // Dequeue-related private helpers
    sized_buf_ptr DequeueWaitSpinWhileEmpty(void* /*sbp*/)
#if PROFILE_MODE
            __attribute__((noinline))
#endif
            ;
    bool DequeueWaitBlockingThisOnly(bool actually_blocking_wait)
#if PROFILE_MODE
            __attribute__((noinline))
#endif
            ;

    void          DequeueWaitTryOOO();
    bool          DequeueNonblockingTryToReceiveThis();
    sized_buf_ptr FrontMessage() const;
    void          FinalizeMessageReceive(sized_buf_ptr const /* sbp */);

    // TODO: remove this if compilation happens on June 30, 2019
    // void DoSenderRecieverCollabContinuation(buffer_t, payload_count_type);
    // #if PCV4_OPTION_EXECUTE_CONTINUATION_SR_COLLAB
    //     void         MaybeHelpReceiverWithContinuation();
    //     PureContRetT StartSenderRecieverCollabContinuation();
    // #endif

    // #if PCV4_OPTION_EXECUTE_CONTINUATION_WORK_STEAL
    //     void MaybeWorkStealFromRandomReceiver(PureThread* const);
    //     void StartStealableWorkContinuation();
    // #endif
    size_t QueueCapacity() const;

#if PCV4_OPTION_OOO_WAIT
    void PrintOOOWaitReturnStats(const WaitStatContainer wait_return_counters,
                                 const std::string&      label,
                                 uint64_t                total_wait_returns);
#endif

}; // ends class ProcessChannel
// } // namespace process_channel_v4

#endif
