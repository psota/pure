// Author: James Psota
// File:   process_channel_v4.cpp 

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

#include "pure/transport/process_channel_v4.h"
#include "pure/runtime/pure_thread.h"
#include "pure/support/benchmark_timer.h"
#include "pure/support/typed_producer_consumer_queue.h"
#include "pure/support/untyped_producer_consumer_queue.h"
#include "pure/transport/recv_channel.h"
#include "pure/transport/send_channel.h"
#include <inttypes.h>
#include <limits>

#define PC_DEBUG_PRINT 0
#if DEBUG_CHECK && PC_DEBUG_PRINT
#define pc_debug_print(M, ...) fprintf(stderr, KYEL M KRESET, ##__VA_ARGS__)
#else
#define pc_debug_print(M, ...)
#endif

#if PROCESS_CHANNEL_STATS
#define PC_STAT_INC(var) ++var
#else
#define PC_STAT_INC(var)
#endif

#if COLLECT_THREAD_TIMELINE_DETAIL
#define PC_SEND_START_TIMER(t) send_pure_thread_->t.start();
#define PC_SEND_PAUSE_TIMER(t) \
    send_pure_thread_->t.pause(send_channel_->GetDestPureRank(), this);
#define PC_RECV_START_TIMER(t) recv_pure_thread_->t.start();
#define PC_RECV_PAUSE_TIMER(t) \
    recv_pure_thread_->t.pause(recv_channel_->GetSenderPureRank(), this);
#else
#define PC_SEND_START_TIMER(t)
#define PC_SEND_PAUSE_TIMER(t)
#define PC_RECV_START_TIMER(t)
#define PC_RECV_PAUSE_TIMER(t)
#endif

#if PCV3_OPTION_ACTIVE_PAUSE
#define ACTIVE_PAUSE_IF_ACTIVATED x86_pause_instruction()
#else
#define ACTIVE_PAUSE_IF_ACTIVATED
#endif

// namespace process_channel_v4 {

// called either by sender thread or receiver thread via PureProcess. only gets
// called once per sender/receiver pair.
ProcessChannel::ProcessChannel(int  num_payload_bytes_arg,
                               bool use_buffered_chan, size_t max_buffered_msgs)
    : num_payload_bytes_(num_payload_bytes_arg),
      // bytes_to_copy_enqueue_(num_payload_bytes_arg),
      using_rt_recv_buf_(use_buffered_chan) {
    static_assert(PCV_4_NUM_CONT_CHUNKS <=
                  std::numeric_limits<chunk_id_t>::max());

    if (using_rt_recv_buf_) {
        // allocate the buffered_payloads_ structure
        buffered_payloads_ = new UntypedProducerConsumerQueue(
                max_buffered_msgs + 1, num_payload_bytes_);
        buffered_dequeue_dest_bufs.set_capacity(1);
    } else {
        dequeue_requests_ = new ProducerConsumerQueue<DequeueReq, true>(
                max_buffered_msgs + 1);
        deq_reqs_payload_counts_ =
                new ProducerConsumerQueue<int>(max_buffered_msgs + 1);
    }
}

// called once by PureProcess inside PureProcess destructor
ProcessChannel::~ProcessChannel() {

    assert(buffered_payloads_ == nullptr || dequeue_requests_ == nullptr);

    const bool using_rt_send_buf = send_buf_aligned_;
    if (using_rt_send_buf) {
        std::free(send_buf_);
    }

    // first cache line for the sender and receiver thread, respectively
    PureRT::assert_cacheline_aligned(&buffered_payloads_);
    PureRT::assert_cacheline_aligned(&send_pure_thread_);
#if PCV4_OPTION_EXECUTE_CONTINUATION
    PureRT::assert_cacheline_aligned(&deq_pipeline);
#endif

#if PROCESS_CHANNEL_STATS
    fprintf(stderr, KBOLD KLT_PUR "  PC Stats [r%d -> r%d] (pcv: %p) ",
            send_channel_->GetSenderPureRank(),
            recv_channel_->GetDestPureRank(), this);

    if (using_rt_recv_buf_) {
        fprintf(stderr, "   [BUFFERED CHANNEL]");
    } else {
        fprintf(stderr, "   [RENDEZVOUS CHANNEL]");
    }
    fprintf(stderr, "______________________\n" KRESET);
    fprintf(stderr,
            "• Enqueue iterations where queue was full for buffered (or empty "
            "(no deq request available for memcpy) for nonbuffered): %" PRIu64
            " / %" PRIu64 " enqueue calls total\n",
            stat_enqueue_queue_full_spin_loops, stat_num_enqueues);
    fprintf(stderr,
            "• Dequeue wait iterations where queue was empty: %" PRIu64
            " / %" PRIu64 " "
            "wait calls total\n ",
            stat_deq_wait_queue_empty_spin_loops, stat_num_deq_waits);
    fprintf(stderr, "  ⤷ %.2f spin iterations/Waits\n",
            static_cast<double>(stat_deq_wait_queue_empty_spin_loops) /
                    static_cast<double>(stat_num_deq_waits));
#if PCV4_OPTION_OOO_WAIT
    fprintf(stderr,
            "• Successful... OOO enqueues: %" PRIu64 "\tOOO dequeues: %" PRIu64
            "\n",
            stat_ooo_enqueues, stat_ooo_dequeues);
    fprintf(stderr,
            "• Wait no message avail OOO pending dequeue probes: %" PRIu64
            " (%" PRIu64 " "
            "wait calls total)\n",
            stat_deq_wait_empty_scan_iterations, stat_num_deq_waits);
#endif
    fprintf(stderr,
            "• ~Max in flight queue size: %u "
            "(usable capacity: %d "
            "(%.1f%% used))\trc: %p\tpayload bytes: %d\n",
            stat_buffered_payloads_max_sz, QueueCapacity(),
            percentage<uint64_t>(stat_buffered_payloads_max_sz,
                                 QueueCapacity()),
            recv_channel_, num_payload_bytes_);

    fprintf(stderr,
            "%d\t%d\t%zu\t%d\tints\tmax queue "
            "size\tcapacity\tpayload_bytes\taaaaa\n",
            num_payload_bytes_ / 4, stat_buffered_payloads_max_sz,
            QueueCapacity(), num_payload_bytes_);

    check(stat_num_enqueues == stat_num_dequeues,
          "expected num enqueues (%ld) to be the same as num dequeues (%ld)",
          stat_num_enqueues, stat_num_dequeues);

#if PCV4_OPTION_OOO_WAIT
    const auto total_enq_wait_rets =
            stat_enq_wait_ret_mrd_1 + stat_enq_wait_ret_nbt_2 +
            stat_enq_wait_ret_one_pe_3 + stat_enq_wait_ret_scanned_this_4 +
            stat_enq_wait_ret_extra_try_5;
    assert(total_enq_wait_rets == stat_num_enq_waits);

    WaitStatContainer wait_return_counters;
    wait_return_counters.reserve(5);

    wait_return_counters.push_back(
            std::make_pair("ret_mrd_1", stat_enq_wait_ret_mrd_1));
    wait_return_counters.push_back(
            std::make_pair("ret_nbt_2", stat_enq_wait_ret_nbt_2));
    wait_return_counters.push_back(
            std::make_pair("ret_one_pe_3", stat_enq_wait_ret_one_pe_3));
    wait_return_counters.push_back(std::make_pair(
            "ret_scanned_this_4", stat_enq_wait_ret_scanned_this_4));
    wait_return_counters.push_back(
            std::make_pair("ret_extra_try_5", stat_enq_wait_ret_extra_try_5));
    assert(wait_return_counters.size() == 5);
    PrintOOOWaitReturnStats(wait_return_counters, "Enqueue",
                            total_enq_wait_rets);
    ////////
    uint64_t total_deq_wait_rets =
            stat_deq_wait_ret_mrd_1 + stat_deq_wait_ret_nbt_2 +
            stat_deq_wait_ret_one_pd_3 + stat_deq_wait_ret_scanned_this_4 +
            stat_deq_wait_ret_extra_try_5;
    assert(total_deq_wait_rets == stat_num_deq_waits);

    wait_return_counters.clear();
    wait_return_counters.push_back(
            std::make_pair("ret_mrd_1", stat_deq_wait_ret_mrd_1));
    wait_return_counters.push_back(
            std::make_pair("ret_nbt_2", stat_deq_wait_ret_nbt_2));
    wait_return_counters.push_back(
            std::make_pair("ret_one_pd_3", stat_deq_wait_ret_one_pd_3));
    wait_return_counters.push_back(std::make_pair(
            "ret_scanned_this_4", stat_deq_wait_ret_scanned_this_4));
    wait_return_counters.push_back(
            std::make_pair("ret_extra_try_5", stat_deq_wait_ret_extra_try_5));
    assert(wait_return_counters.size() == 5);

    PrintOOOWaitReturnStats(wait_return_counters, "Dequeue",
                            total_deq_wait_rets);
#endif
    fprintf(stderr,
            KGREY "_____________________________________________\n" KRESET);
#endif

    // free up the main message queue
    if (using_rt_recv_buf_) {
        const size_t sz0 = buffered_payloads_->sizeGuess();
        check(sz0 == 1,
              "Upon finishing using this ProcessChannel (v4) the "
              "buffered_payloaded SPSC queue should have just a single entry "
              "in "
              "it, assuming the number of receives exactly matched the "
              "number of sends. However, there are %zu entries on "
              "it. It's 1 and not 0 because the popFront call only gets made "
              "on a "
              "subsequent Dequeue.",
              sz0);
        delete buffered_payloads_;
        // the last thing should be a Wait, and that should keep this flag as
        // true
        assert(ok_to_pop_buffer);
    } else {
        delete dequeue_requests_;
        delete deq_reqs_payload_counts_;
    }
}

#if PCV4_OPTION_OOO_WAIT
void ProcessChannel::PrintOOOWaitReturnStats(
        const WaitStatContainer wait_return_counters, const std::string& label,
        uint64_t total_wait_returns) {
    fprintf(stderr,
            "• [%" PRIu64 " %s Wait Returns] STATS: ", total_wait_returns,
            label.c_str());
    for (auto const c : wait_return_counters) {
        fprintf(stderr, "%s: %" PRIu64 " (%.1f%%) ", c.first.c_str(), c.second,
                percentage<uint64_t>(c.second, total_wait_returns));
    }
    fprintf(stderr, "\n");
}
#endif

// This is called after construction in PureProcess::BindToProcessChannel if
// using the runtime send buffer. Otherwise it's assumed that the user buffer is
// used.
void ProcessChannel::AllocateSendRTBuffer() {
    assert(send_buf_ == nullptr);
    send_buf_aligned_ = true; // only the sender has this field, and only the
                              // sender calls this function. So, have the sender
                              // set this now.
    // override bytes_to_copy_enqueue_ now that we know it's using the send
    // buffer
    // bytes_to_copy_enqueue_ = jp_memory_alloc<1, CACHE_LINE_BYTES, true>(
    //         &send_buf_, num_payload_bytes_);
    // send_buf_ = jp_memory_alloc<1, CACHE_LINE_BYTES,
    // true>(num_payload_bytes_);

    cache_line_rounded_bytes_to_copy_ =
            jp_memory_alloc<1, CACHE_LINE_BYTES, true>(&send_buf_,
                                                       num_payload_bytes_);
    assert(cache_line_rounded_bytes_to_copy_ % CACHE_LINE_BYTES == 0);
    assert(send_buf_ != nullptr);
    // assert(bytes_to_copy_enqueue_ % CACHE_LINE_BYTES == 0);
    PureRT::assert_cacheline_aligned(send_buf_);
    using_rt_send_buf_ = true;
}

void ProcessChannel::BindSendChannel(SendChannel* const sc) {
    // make sure this is called just once because the SendChannel dictates
    // buffer sizes, etc, and this ProcessChannel was already set up in the
    // constructor.
    check(send_channel_ == nullptr,
          "You're binding a send channel to a ProcessChannel that already has "
          "a bound send_channel. This likely means that you are initializing "
          "multiple send channels with the same parameters, and the new send "
          "channel (this one) is binding to the old one. You likely need to "
          "either just use the existing channel or disambiguate with a tag or "
          "something. This could come up with wrapper classes such as "
          "ScanChannel which internally allocate channels. Be careful about "
          "the tags not colliding.");

    send_channel_     = sc;
    send_pure_thread_ = send_channel_->GetPureThread();
#if PCV4_OPTION_OOO_WAIT
    pending_enqueue_mgr = &send_pure_thread_->pc_enqueue_mgr;
#endif

    // note: it's possible that send_buf_ is nullptr if using_rt_send_buf_hint
    // in PureProcess was false.
    send_channel_->SetSendChannelBuf(send_buf_);
    send_channel_->SetInitializedState();
}

void ProcessChannel::BindRecvChannel(RecvChannel* const rc) {
    assert(recv_channel_ == nullptr);
    recv_channel_ = rc;
    assert(recv_channel_->GetRawRecvChannelBuf() == nullptr);

    // initialize the receiver buffer if receiver initializtion function was
    // set note: should probably do this for the sender buffer now.
    if (recv_channel_->HasBufferInitializerFunc()) {
        // initialize just the first buffer slot. the other buffer slots
        // will be written by send.
        sentinel("For ProcessChannel, buffer initializer functions are not "
                 "implemented. If you want to do this, "
                 "you'll have to somehow initialize the first buffer entry "
                 "(which hasn't been allocated yet).");
    }
    recv_pure_thread_ = recv_channel_->GetPureThread();

#if PCV4_OPTION_OOO_WAIT
    // create a direct path to the channel manager so we don't have to traverse
    // through the thread
    pending_dequeue_mgr = &recv_pure_thread_->pc_dequeue_mgr;
#endif
    recv_channel_->SetInitializedState();
}

#if PCV4_OPTION_EXECUTE_CONTINUATION
void ProcessChannel::SetDeqContPipeline(PurePipeline&& p) {
    // should use the move assignment
    // this is currenty broken

    // TODO: enable this assertion when this is only called when the
    // continuation is there assert(!deq_pipeline.Empty());
    if (p.NumStages() > 0) {
        pipeline_initialized       = true;
        deq_pipeline               = std::move(p);
        pipeline_has_collab_stages = deq_pipeline.HasAnyCollabStage();
    }
}

#endif

//// SENDING FUNCTIONS ///////////////////////////////////////////////////

/*
 * Basic Enqueue Algorithm:

Note - 2021: OOO approach is not working -- ignore for now.

 *   EnqueueImpl: if buffer is available, sends immediately and sets
 * message_send_done to true; else, inserts this ProcessChannel into
 * pcs_with_pending_enqueues
 *
 *   EnqueueSpinUntilNotFullAndSend: if buffer available, sends immediately and
 * returns; else cycle through all pcs_with_pending_enqueues, trying to do the
 * send for any of them that have space. Return as soon as this PC send is
 * complete.
 */

// uses runtime buffer
void ProcessChannel::Enqueue(const size_t count_to_send) {
    assert(message_send_done == false);
    assert(send_buf_ != nullptr);
    // enq_count_to_send_ = count_to_send;
    EnqueueImpl(send_buf_, count_to_send);
}

// uses user buffer
void ProcessChannel::EnqueueUserBuf(const buffer_t __restrict user_buf,
                                    const size_t count_to_send) {
    assert(message_send_done == false);
    check(using_rt_send_buf_ == false,
          "using_rt_send_buf_ is set to false, but you are using "
          "EnqueueUserBuf. You may want to set using_rt_send_buf_hint to false "
          "during the call to InitSendChannel.");
    // this is set upon every call to EnqueueUserBuf, as the buffer address may
    // change throughout the course of the program
    // send_buf_          = user_buf;
    // enq_count_to_send_ = count_to_send;
    EnqueueImpl(user_buf, count_to_send);
}

// Called only by sender thread
void ProcessChannel::EnqueueImpl(const buffer_t buf, const size_t count) {
    PC_SEND_START_TIMER(pc_enqueue_end_to_end);

    assert(count <= send_channel_->GetCount());
    // try to send (copy) message now if space is available. don't spin.
    const bool copied_buf = EnqueueNonblockingTryToSendThis(buf, count);

    if (copied_buf) {
        message_send_done = true;
    } else {
        // save these for the wait
        send_buf_          = buf;
        enq_count_to_send_ = count;
#if PCV4_OPTION_OOO_WAIT
        assert(message_send_done == false);
        pending_enqueue_index_upon_enqueue =
                pending_enqueue_mgr->AddPendingQueue(this);
#endif
    }

#if PROCESS_CHANNEL_STATS
    if (using_rt_recv_buf_) {
        const auto s = buffered_payloads_->sizeGuess();
        if (s > stat_buffered_payloads_max_sz) {
            stat_buffered_payloads_max_sz = s;
        }
    }
#endif

    PC_SEND_PAUSE_TIMER(pc_enqueue_end_to_end);
    PC_STAT_INC(stat_num_enqueues);
}

// blocking
void ProcessChannel::EnqueueBlocking(const size_t count_to_send) {
    PC_SEND_START_TIMER(pc_enqueue_end_to_end);
    assert(message_send_done == false);
    enq_count_to_send_ = count_to_send;
    EnqueueWaitBlockingThisOnly();
    PC_SEND_PAUSE_TIMER(pc_enqueue_end_to_end);
    PC_STAT_INC(stat_num_enqueues);
}

// blocking
void ProcessChannel::EnqueueBlockingUserBuf(const buffer_t __restrict user_buf,
                                            const size_t count_to_send) {
    PC_SEND_START_TIMER(pc_enqueue_end_to_end);
    assert(message_send_done == false);
    send_buf_          = user_buf;
    enq_count_to_send_ = count_to_send;
    EnqueueWaitBlockingThisOnly();
    PC_SEND_PAUSE_TIMER(pc_enqueue_end_to_end);
    PC_STAT_INC(stat_num_enqueues);
}

size_t ProcessChannel::BytesToCopy(int count) const {
    // this gets set when send buf is allocated

    // Experimental: not using aligned copies at this time.
    // cache_line_rounded_bytes_to_copy_ is ignored.

    // const bool
    // using_rt_send_buf = send_buf_aligned_; return using_rt_send_buf ?
    // cache_line_rounded_bytes_to_copy_
    //                          : count * send_channel_->GetDatatypeBytes();
    return count * send_channel_->GetDatatypeBytes();
}

bool ProcessChannel::EnqueueNonblockingTryToSendThis(const buffer_t send_buf,
                                                     const size_t   count) {
    // We need to handle both buffered and non-buffered channels
    if (using_rt_recv_buf_) {
        // Case 1: Buffered messages
        // assert(enq_count_to_send_ * send_channel_->GetDatatypeBytes() ==
        //        bytes_to_copy_enqueue_);
        const size_t bytes_to_copy = BytesToCopy(count);
        return buffered_payloads_->writeAndCopyBuf(
                send_buf, send_buf_aligned_, count, bytes_to_copy
#if COLLECT_THREAD_TIMELINE_DETAIL
                ,
                send_pure_thread_->pc_memcpy_timer
#endif
        );
    } else {
        // Case 2: Non-Buffered Messages
        DequeueReq req;
        const bool deq_req_avail = dequeue_requests_->read(req);

        if (deq_req_avail) {
            FinalizeUnbufferedSend(send_buf, count, req.dest);
            return true;
        } else {
            // no request available
            return false;
        }
    }
}

void ProcessChannel::FinalizeUnbufferedSend(
        const buffer_t __restrict source_buf, const size_t enq_count,
        buffer_t __restrict dest) {
    // A request from the receiver is available, so place the message where
    // the receiver has requested it and mark it as done

    // size to copy: for now, make sure the sender is sending at least as
    // many bytes as the receiver needs
    check(enq_count * send_channel_->GetDatatypeBytes() <=
                  recv_channel_->NumPayloadBytes(),
          "Sender wants to send %lu bytes, but the receiver only configured "
          "this channel to have %d bytes.",
          enq_count_to_send_ * send_channel_->GetDatatypeBytes(),
          recv_channel_->NumPayloadBytes());
    assert(send_channel_->GetDatatypeBytes() ==
           recv_channel_->GetDatatypeBytes());
    MEMCPY_IMPL(dest, source_buf,
                enq_count * send_channel_->GetDatatypeBytes());

    // insert payload count for this message that it's sending into the payload
    // count queue
    const auto write_succeeded = deq_reqs_payload_counts_->write(enq_count);
    check(write_succeeded,
          "deq_reqs_payload_counts_ is full. the max_buffered_msgs parameter "
          "(%lu) to this ProcessChannel is likely too small",
          deq_reqs_payload_counts_->capacity());
}

/////////////////////////////////////////////////////////////////

void ProcessChannel::EnqueueWait() {
#if PCV4_OPTION_OOO_WAIT
    EnqueueWaitTryOOO();
#else
    EnqueueWaitBlockingThisOnly();
#endif

#if PCV4_OPTION_EXECUTE_CONTINUATION_SR_COLLAB
    // N.B. this timer here is just to make the math work out with the timer
    // interval utility calculation. Maybe there's a cleaner way tot do this.
    if (pipeline_has_collab_stages) {
        // only help if we know that the pipeline has at least one collaborative
        // stage
        PC_SEND_START_TIMER(pc_enq_wait_sr_collab_parent_wrapper);
        deq_pipeline.MaybeHelpReceiverWithContinuation(send_pure_thread_);
        PC_SEND_PAUSE_TIMER(pc_enq_wait_sr_collab_parent_wrapper);
    }
#endif

    PC_STAT_INC(stat_num_enq_waits);
    message_send_done = false; // reset for next time
}

bool ProcessChannel::EnqueueTest() {
    const auto actually_blocking_wait = false;
    const auto enqueue_succeeded      = EnqueueTryTest();
    PC_STAT_INC(stat_num_enq_waits);

    if (enqueue_succeeded) {
        message_send_done = false; // reset for next time
    }
    return enqueue_succeeded;
}

#if PCV4_OPTION_EXECUTE_CONTINUATION_SR_COLLAB
// N.B. this timer here is just to make the math work out with the timer
// interval utility calculation. Maybe there's a cleaner way tot do this.
if (pipeline_has_collab_stages) {
    // only help if we know that the pipeline has at least one collaborative
    // stage
    PC_SEND_START_TIMER(pc_enq_wait_sr_collab_parent_wrapper);
    deq_pipeline.MaybeHelpReceiverWithContinuation(send_pure_thread_);
    PC_SEND_PAUSE_TIMER(pc_enq_wait_sr_collab_parent_wrapper);
}
#endif

void ProcessChannel::EnqueueWaitBlockingThisOnly() {

    // todo use actually blocking var
    if (!message_send_done) {
        PC_SEND_START_TIMER(pc_enqueue_wait_this_only);

        if (using_rt_recv_buf_) {
            /////////////////////////////////////////
            // Case 1: Buffered messages
            // assert(enq_count_to_send_ * send_channel_->GetDatatypeBytes() ==
            //        bytes_to_copy_enqueue_);
            while (!buffered_payloads_->writeAndCopyBuf(
                    send_buf_, send_buf_aligned_, enq_count_to_send_,
                    enq_count_to_send_ * send_channel_->GetDatatypeBytes()
#if COLLECT_THREAD_TIMELINE_DETAIL
                            ,
                    send_pure_thread_->pc_memcpy_timer
#endif
                    )) {

                ACTIVE_PAUSE_IF_ACTIVATED;
                PC_STAT_INC(stat_enqueue_queue_full_spin_loops);
                MAYBE_WORK_STEAL(send_pure_thread_);
            }
        } else {
            /////////////////////////////////////////
            // Case 2: Non-Buffered Messages
            DequeueReq req;
            while (!(dequeue_requests_->read(req))) {
                ACTIVE_PAUSE_IF_ACTIVATED;
                // actually this stat is that the queue was empty
                PC_STAT_INC(stat_enqueue_queue_full_spin_loops);
                MAYBE_WORK_STEAL(send_pure_thread_);
            } // while the dequeue request is not available

            // Ok, req is now filled with a valid dequeue request, so send it
            // (deposit data into receiver's desired buffer).
            FinalizeUnbufferedSend(send_buf_, enq_count_to_send_, req.dest);
        }

        // at this point, regardless of if it was a buffered or non-buffered
        // send, it's done.
        PC_SEND_PAUSE_TIMER(pc_enqueue_wait_this_only);
    } // if not message_send_done
}

bool ProcessChannel::EnqueueTryTest() {

    if (message_send_done) {
        // note that we reset message_send_done in the caller
        return true;
    } else {
        // try to send ONCE
        PC_SEND_START_TIMER(pc_enqueue_wait_this_only);

        bool enqueue_try_succeeded = false;
        if (using_rt_recv_buf_) {
            /////////////////////////////////////////
            // Case 1: Buffered messages
            enqueue_try_succeeded = buffered_payloads_->writeAndCopyBuf(
                    send_buf_, send_buf_aligned_, enq_count_to_send_,
                    enq_count_to_send_ * send_channel_->GetDatatypeBytes()
#if COLLECT_THREAD_TIMELINE_DETAIL
                            ,
                    send_pure_thread_->pc_memcpy_timer
#endif
            );
        } else {
            /////////////////////////////////////////
            // Case 2: Non-Buffered Messages
            DequeueReq req;
            enqueue_try_succeeded = dequeue_requests_->read(req);
            if (enqueue_try_succeeded) {
                // Ok, req is now filled with a valid dequeue request, so send
                // it
                // (deposit data into receiver's desired buffer).
                FinalizeUnbufferedSend(send_buf_, enq_count_to_send_, req.dest);
            }
        }

        PC_SEND_PAUSE_TIMER(pc_enqueue_wait_this_only);
        return enqueue_try_succeeded;
    } // if message_send_done is false
}

#if PCV4_OPTION_OOO_WAIT
void ProcessChannel::EnqueueWaitTryOOO() {

    // Exit path 1: already sent (by another Enqueue Wait)
    if (message_send_done) {
        PC_STAT_INC(stat_enq_wait_ret_mrd_1);
        return;
    }

    // Exit path 2: ready to send now
    if (EnqueueNonblockingTryToSendThis()) {
        pending_enqueue_mgr->RemoveMatchingQueueCandidate(
                this, pending_enqueue_index_upon_enqueue);
        pending_enqueue_mgr->VerifyNoMembership(this);
        PC_STAT_INC(stat_enq_wait_ret_nbt_2);
        return;
    }

    PC_SEND_START_TIMER(pc_enq_wait_probe_pending_enqueues);
    for (;;) {
        const auto num_pending_enqueues =
                pending_enqueue_mgr->NumPendingQueues();
        if (num_pending_enqueues == 1) {
            // just me -- fast path. at this point (assuming no work stealing is
            // going on) we know there won't be any other pending enqueues added
            // to this thread. so we have to wait until we are done sending
            // this desired message.
            PC_SEND_PAUSE_TIMER(pc_enq_wait_probe_pending_enqueues);
            EnqueueWaitBlockingThisOnly();
            assert(pending_enqueue_mgr->NumPendingQueues() == 1);
            pending_enqueue_mgr->EmptyPendingQueueCandidates();
            pending_enqueue_mgr->VerifyNoMembership(this);
            PC_STAT_INC(stat_enq_wait_ret_one_pe_3);
            return;
        } else {
            // look for other process channels to send -- but note that this
            // list includes myself (this) so we don't have to specially process
            // it -- it will happen once through the list
            check(num_pending_enqueues <= 64,
                  "Assumed num_pending_enqueues was less than 64, but "
                  "it is %zu",
                  num_pending_enqueues);
            for (auto attempt = 0; attempt < num_pending_enqueues; ++attempt) {
                int const  idx = num_pending_enqueues - 1 - attempt;
                auto const pc = pending_enqueue_mgr->PendingQueueCandidate(idx);

                if (pc->EnqueueNonblockingTryToSendThis()) {
                    pending_enqueue_mgr->RemovePendingQueueCandidate(idx);
                    pending_enqueue_mgr->VerifyNoMembership(pc);
                    PC_STAT_INC(stat_ooo_enqueues);
                    if (pc == this) {
                        // exit if this is the ProcessChannel we want (which is
                        // guaranteed to be on the pending enqueue list)
                        PC_SEND_PAUSE_TIMER(pc_enq_wait_probe_pending_enqueues);
                        PC_STAT_INC(stat_enq_wait_ret_scanned_this_4);
                        return;
                    } else {
                        // successful enqueue, but nFinalizeMessageSendDone();
                        // // mark as received
                        pc->message_send_done = true;

                        // now that we spent time doing an OOO send, check
                        // desired (this) process channel again
                        if (this->EnqueueNonblockingTryToSendThis()) {
                            pending_enqueue_mgr->RemoveMatchingQueueCandidate(
                                    this, pending_enqueue_index_upon_enqueue);
                            pending_enqueue_mgr->VerifyNoMembership(this);
                            PC_SEND_PAUSE_TIMER(
                                    pc_enq_wait_probe_pending_enqueues);
                            PC_STAT_INC(stat_enq_wait_ret_extra_try_5);
                            return;
                        }
                    }
                } else {
                    PC_STAT_INC(stat_enq_wait_empty_scan_iterations);
                }

            } // ends for loop through the pending enqueues list
        } // ends else case where there are some other pending enqueues (other
          // than this one) to try to send out of order
        MAYBE_WORK_STEAL(send_pure_thread_);
        // #if PCV4_OPTION_EXECUTE_CONTINUATION_WORK_STEAL
        //         send_pure_thread_->MaybeExecuteStolenWorkOfRandomReceiver();
        // #endif
    } // ends infinite loop until desired message sent

    sentinel("Should never get here!");
}

#endif

//// RECEIVING FUNCTIONS ///////////////////////////////////////////
////////////////////////////////////////////////////////////////////

__attribute__((flatten)) void
ProcessChannel::Dequeue(const buffer_t __restrict user_buf) {

    assert(message_receive_done == false);

    if (using_rt_recv_buf_) {
        // Case 1: Buffered messages
        // if there's a user_buf given, store it for later copying from the
        // runtime buffer
        if (user_buf != nullptr) {
            // resize if necessary
            if (buffered_dequeue_dest_bufs.capacity() -
                        buffered_dequeue_dest_bufs.size() ==
                1) {
                buffered_dequeue_dest_bufs.set_capacity(
                        buffered_dequeue_dest_bufs.capacity() * 2);
            }
            const buffer_t user_buf_no_restrict = user_buf;
            buffered_dequeue_dest_bufs.push_back(user_buf_no_restrict);
        } else {
            // if user_buf is nullptr, make sure buffered_dequeue_dest_bufs
            // doesn't have entries in it. See warning below at FinalizeReceive
            assert(buffered_dequeue_dest_bufs.size() == 0);
        }
        if (ok_to_pop_buffer) {
            assert(FrontMessage() != nullptr);
            buffered_payloads_->popFront();
            ok_to_pop_buffer = false;
        }
    } else {
        // Case 2: Non-Buffered Messages
        // need to store the user buf for a later memcpy
        check(user_buf != nullptr,
              "This Process Channel is running in rendezvous mode yet no "
              "user_buf was passed to rc->Dequeue(). Note: this channel's "
              "payloads are %d bytes, but the BUFFERED_CHAN_MAX_PAYLOAD_BYTES "
              "threshold is %d bytes.\n",
              num_payload_bytes_, BUFFERED_CHAN_MAX_PAYLOAD_BYTES);
        assert(buffered_dequeue_dest_bufs.size() == 0);
        const bool write_success = dequeue_requests_->write(user_buf);
        check(write_success,
              "Writing to dequeue_requests in ProcessChannel failed for this"
              "non-buffered channel. That likely means that the queue size"
              " (currently %zu) is too small.",
              QueueCapacity());

#if PRINT_PROCESS_CHANNEL_STATS
        const auto sz = dequeue_requests_->sizeGuess();
        if (sz > stat_buffered_payloads_max_sz) {
            stat_buffered_payloads_max_sz = sz;
        }
#endif
    }

#if PCV4_OPTION_OOO_WAIT
    // add pending dequeue to PureThread structure so OOO waits can be done

    // TODO: make sure duplicate adds aren't made for same channel iwth multi
    // dequeues
    if (outstanding_deqs == 0) {
        pending_dequeue_index_upon_dequeue =
                pending_dequeue_mgr->AddPendingQueue(this);
    }
    ++outstanding_deqs;
    assert(outstanding_deqs < QueueCapacity());
#endif
    PC_STAT_INC(stat_num_dequeues);
}

__attribute__((flatten)) const PureContRetArr&
ProcessChannel::DequeueWait(bool                 actually_blocking_wait,
                            std::optional<bool*> dequeue_succeeded) {

    if (using_rt_recv_buf_) {
        // Case 1: Clean up now-used buffered message
        // dequeue the previous entry
        if (ok_to_pop_buffer) {
            assert(FrontMessage() != nullptr);
            buffered_payloads_->popFront();
            ok_to_pop_buffer = false;
        }
    }

    PC_RECV_START_TIMER(pc_deq_wait_end_to_end);
#if PCV4_OPTION_OOO_WAIT
    DequeueWaitTryOOO();
    --outstanding_deqs;
#else
    const bool did_dequeue =
            DequeueWaitBlockingThisOnly(actually_blocking_wait);
    if (using_rt_recv_buf_ && did_dequeue) {
        assert(ok_to_pop_buffer == false);
        // dequeue succeeded so it's ok to pop (next time)
        ok_to_pop_buffer = true;
    }

    if (dequeue_succeeded.has_value()) {
        *(dequeue_succeeded.value()) = did_dequeue;
    }
#endif
    PC_RECV_PAUSE_TIMER(pc_deq_wait_end_to_end);
    PC_STAT_INC(stat_num_deq_waits);

// this return value is set by some other Wait call which set this value in the
// ProcessChannel member
#if PCV4_OPTION_EXECUTE_CONTINUATION
    return this->deq_pipeline.GetReturnVals();
#else
    // returns a reference to a const empty vector
    return static_empty_pure_cont_ret_arr;
#endif // no continuation
}

__attribute__((flatten)) void
ProcessChannel::DequeueBlocking(const buffer_t __restrict user_buf) {
    Dequeue(user_buf);
    DequeueWait();
}

bool ProcessChannel::DequeueWaitBlockingThisOnly(bool actually_blocking_wait) {
    if (using_rt_recv_buf_) {
        // Case 1: Buffered Messages
        sized_buf_ptr this_sbp = this->FrontMessage();
        if (this_sbp == nullptr) {
            if (actually_blocking_wait) {
                this_sbp = this->DequeueWaitSpinWhileEmpty(this_sbp);
            } else {
                return false;
            }
        }
        assert(this_sbp != nullptr);
        this->FinalizeMessageReceive(this_sbp);
    } else {
        // Case 2: Non-Buffered Messages
        // wait until the number of send messages is more than the number that
        // I've already received
        PC_RECV_START_TIMER(pc_deq_wait_empty_queue_timer);
        int payload_count;
        while (!deq_reqs_payload_counts_->read(payload_count)) {
            if (actually_blocking_wait) {
                ACTIVE_PAUSE_IF_ACTIVATED;
                MAYBE_WORK_STEAL(recv_pure_thread_);
                PC_STAT_INC(stat_deq_wait_queue_empty_spin_loops);
            } else {
                // it tried once and failed, so return
                return false;
            }
        }
        PC_RECV_PAUSE_TIMER(pc_deq_wait_empty_queue_timer);

        assert(payload_count > 0); // I supposed we could allow zero-length
                                   // receives, but we probably shouldn't.
        recv_channel_->SetReceivedCount(payload_count);
    }
    PC_STAT_INC(num_completed_dequeues);
    return true;
}

#if PCV4_OPTION_OOO_WAIT
void ProcessChannel::DequeueWaitTryOOO() {

#if PRINT_PROCESS_CHANNEL_STATS
    struct timespec now_ts;
    utc_time_monotonic(&now_ts);
    // pending_dequeues_this_thread includes ALL pending dequeues, including
    // some from this process channel
    pending_process_recv_channel_snapshots.emplace_back(
            now_ts, pending_dequeue_mgr->NumPendingQueues());
#endif

    if (message_receive_done) {
        // message already received and processed (OOO)
        message_receive_done = false; // reset for next time
        if (NoRemainingOutstandingDeqs()) {
            pending_dequeue_mgr->VerifyNoMembership(this);
        }
        pc_debug_print("ProcessChannel %p: return 1 [==] "
                       "message already received "
                       "\n",
                       this);
        PC_STAT_INC(stat_deq_wait_ret_mrd_1);
        return;
    }

    if (DequeueNonblockingTryToReceiveThis()) {
        pc_debug_print("ProcessChannel %p: return 2 [==] "
                       "DequeueNonblockingTryToReceiveThis succeeded "
                       "\n",
                       this);
        if (NoRemainingOutstandingDeqs()) {
            pending_dequeue_mgr->VerifyNoMembership(this);
        }
        PC_STAT_INC(stat_deq_wait_ret_nbt_2);
        return;
    }

    PC_RECV_START_TIMER(pc_deq_wait_probe_pending_dequeues);
    for (;;) {
        const auto num_pending_deq_chans =
                pending_dequeue_mgr->NumPendingQueues();
        if (num_pending_deq_chans == 1) {
            // just me -- fast path. at this point (assuming no work stealing is
            // going on) we know there won't be any other pending dequeues added
            // to this thread. so we have to wait until we are done receiving
            // this desired message.
            DequeueWaitBlockingThisOnly();
            assert(pending_dequeue_mgr->NumPendingQueues() == 1);
            if (NoRemainingOutstandingDeqs()) {
                pending_dequeue_mgr->EmptyPendingQueueCandidates();
                pending_dequeue_mgr->VerifyNoMembership(this);
            }
            pc_debug_print(
                    "ProcessChannel %p: return 3 [==] WaitBlockingThisOnly "
                    "blocking "
                    "recv\n",
                    this);

            PC_RECV_PAUSE_TIMER(pc_deq_wait_probe_pending_dequeues);
            PC_STAT_INC(stat_deq_wait_ret_one_pd_3);
            return;
        } else {

            // look for others -- but note that this list includes
            // myself (this) so we don't have to specially process it -- it will
            // happen once through the list
            check(num_pending_deq_chans <= 64,
                  "Assumed num_pending_deq_chans was less than 64, but "
                  "it is %zu",
                  num_pending_deq_chans);
            // TODO: measure doing this not in reverse as an optimization
            for (auto attempt = 0; attempt < num_pending_deq_chans; ++attempt) {
                int const             idx = num_pending_deq_chans - 1 - attempt;
                ProcessChannel* const pc =
                        pending_dequeue_mgr->PendingQueueCandidate(idx);
                sized_buf_ptr const curr_sbp = pc->FrontMessage();

                pc_debug_print(
                        "ProcessChannel %p: scanned pending dequeues "
                        "(attempt=%d/num_pending_deq_chans=%d actual "
                        "size is %d) [==] "
                        "and trying PC %p. buf pointer = %p\trecv_chan = %p"
                        "\n",
                        this, attempt, num_pending_deq_chans,
                        pending_dequeue_mgr->NumPendingQueues(), pc, curr_sbp,
                        pc->recv_channel_);
                if (curr_sbp != nullptr) {
                    pc->FinalizeMessageReceive(curr_sbp);
                    if (NoRemainingOutstandingDeqs()) {
                        pending_dequeue_mgr->RemovePendingQueueCandidate(idx);
                        pending_dequeue_mgr->VerifyNoMembership(pc);
                    }
                    PC_STAT_INC(stat_ooo_dequeues);

                    if (pc == this) {
                        // exit if this is the ProcessChannel we want (which is
                        // guaranteed to be on the pending dequeue list)
                        pc_debug_print("ProcessChannel %p: return 4 [==] "
                                       "scanned pending and found this\n",
                                       this);
                        PC_RECV_PAUSE_TIMER(pc_deq_wait_probe_pending_dequeues);
                        PC_STAT_INC(stat_deq_wait_ret_scanned_this_4);
                        return;
                    } else {
                        // mark this as done for when that is actually received
                        pc->message_receive_done = true;

                        // now that we spent time doing an OOO receive, check
                        // desired (this) process channel again
                        if (this->DequeueNonblockingTryToReceiveThis()) {
                            pc_debug_print(
                                    "ProcessChannel %p: return 5 [==] "
                                    "extra try upon receive and this was ready"
                                    "\n",
                                    this);
                            if (NoRemainingOutstandingDeqs()) {
                                pending_dequeue_mgr->VerifyNoMembership(this);
                            }
                            PC_RECV_PAUSE_TIMER(
                                    pc_deq_wait_probe_pending_dequeues);
                            PC_STAT_INC(stat_deq_wait_ret_extra_try_5);
                            return;
                        }
                    } // ends special check to see if this PC is now ready for
                      // receive
                } else {
                    // there wasn't a message to be received from that
                    // ProcessChannel
                    PC_STAT_INC(stat_deq_wait_empty_scan_iterations);
                    // TODO: add back this pause? we are not spin looping so
                    // seems possibly
                    // gratuitous
                    // #if PCV3_OPTION_ACTIVE_PAUSE
                    //                     x86_pause_instruction();
                    // #endif
                } // ends else case where no message available on current
                  // message
            }     // ends for loop through the pending dequeues list
        } // ends else case where there are some other pending dequeues (other
          // than this one) to try to receive out of order

        // went through all of the pending dequeues, so try to steal some work
        // #if PCV4_OPTION_EXECUTE_CONTINUATION_WORK_STEAL
        //         recv_pure_thread_->MaybeExecuteStolenWorkOfRandomReceiver();
        // #endif
        MAYBE_WORK_STEAL(recv_pure_thread_);
    } // ends infinite for loop until desired message received
    sentinel("Should never get here!");
}

bool ProcessChannel::DequeueNonblockingTryToReceiveThis() {
    sized_buf_ptr const this_sbp = this->FrontMessage();
    if (this_sbp != nullptr) {
        assert(message_receive_done == false);
        this->FinalizeMessageReceive(this_sbp);
        if (NoRemainingOutstandingDeqs()) {
            pending_dequeue_mgr->RemoveMatchingQueueCandidate(
                    this, pending_dequeue_index_upon_dequeue);
        }
        return true; // succeeded in receiving
    } else {
        return false; // failed in receiving
    }
}
#endif

sized_buf_ptr ProcessChannel::FrontMessage() const {
    // get the pointer to the buffer where the message resides from
    // buffered payloads.
    return buffered_payloads_->frontPtr();
}

sized_buf_ptr ProcessChannel::DequeueWaitSpinWhileEmpty(sized_buf_ptr sbp) {
    // this should only be called when sbp is nullptr. call FrontMessage first.
    PC_RECV_START_TIMER(pc_deq_wait_empty_queue_timer);
    while (sbp == nullptr) {
        ACTIVE_PAUSE_IF_ACTIVATED;
        MAYBE_WORK_STEAL(recv_pure_thread_);
        PC_STAT_INC(stat_deq_wait_queue_empty_spin_loops);
        sbp = FrontMessage();
    }
    PC_RECV_PAUSE_TIMER(pc_deq_wait_empty_queue_timer);
    return sbp;
}

void ProcessChannel::FinalizeMessageReceive(sized_buf_ptr const sbp) {

    // step -1: error checking
    assert(message_receive_done == false);
    assert(sbp != nullptr);

    // step 1: store this count and pointer in the RecvChannel
    // The data structure returned from frontPtr has the following form:
    //
    //    [ payload_count    |  void* buf ]
    //            ↑                 ↑
    //         uint32_t
    //         bytes_per_payload_buf
    //   .................... or less if the enqueue gave a smaller amount
    const auto payload_count = static_cast<uint32_t*>(sbp)[0];
    assert(payload_count > 0); // I supposed we could allow zero-length
                               // receives, but we probably shouldn't.
    recv_channel_->SetReceivedCount(payload_count);

    const buffer_t __restrict buf_ptr =
            static_cast<uint8_t*>(sbp) + buf_ptr_offset;
    recv_channel_->SetRecvChannelBuf(buf_ptr);

#if PCV4_OPTION_EXECUTE_CONTINUATION
    if (pipeline_initialized) {
        PC_RECV_START_TIMER(pc_deq_wait_continuation);
        // store the reference to the returned pipeline (which never changes
        // throughout the coruse of the program, so optimize by just storing
        // this once)
        // deq_pipe_stage_ret_vals = deq_pipeline.ExecutePCPipeline(buf_ptr,
        // payload_count);

        // note: the pipeline stores the return values directly into its
        // internal structures; those return values are gettable later.
        deq_pipeline.ExecutePCPipeline(buf_ptr, payload_count);
        PC_RECV_PAUSE_TIMER(pc_deq_wait_continuation);
    }
#endif

    // now that the pipeline is done on the runtime buffer, which possibly
    // modified it, copy the payload into the user buffer
    // WARNING: this is really sketchy -- a BUFFERED channel must either ALWAYS
    // use a user buffer or NOT use a user buffer. If it mixes and matches using
    // user buffers on the same channel, the queue structure will get out of
    // sync. It always drains entries from the user buf queue when they are
    // there.
    if (using_rt_recv_buf_) {
        if (buffered_dequeue_dest_bufs.size() > 0) {
            const buffer_t __restrict& __restrict user_buf =
                    buffered_dequeue_dest_bufs.front();

            // size to copy: fundamentally, copy in what the sender wants to
            // send. Just make sure there's enough space for it.
            assert(send_channel_->GetDatatypeBytes() ==
                   recv_channel_->GetDatatypeBytes());
            assert(payload_count * send_channel_->GetDatatypeBytes() <=
                   recv_channel_->NumPayloadBytes());
            MEMCPY_IMPL(user_buf, buf_ptr,
                        payload_count * recv_channel_->GetDatatypeBytes());
            buffered_dequeue_dest_bufs.pop_front();
        }
    }
} // ends FinalizeMessageReceive

//////////////////////////////////////////////////////////////////////////////////

// *** probably keep here. mark as debug functions

size_t ProcessChannel::QueueCapacity() const {
    if (using_rt_recv_buf_) {
        const auto s = buffered_payloads_->capacity();
        assert(s > 0);
        return s;
    } else {
        const auto s = dequeue_requests_->capacity();
        assert(s > 0);
        return s;
    }
}

#if PRINT_PROCESS_CHANNEL_STATS
int ProcessChannel::GetReceiverRank() const {
    return recv_channel_->GetDestPureRank();
}
int ProcessChannel::GetSenderRank() const {
    return send_channel_->GetSenderPureRank();
}
int ProcessChannel::GetReceiverCpu() const {
    return recv_channel_->GetPureThread()->GetCpuId();
}

void ProcessChannel::PrintSenderReceiver() const {
    printf("r%d -> r%d", send_channel_->GetSenderPureRank(),
           recv_channel_->GetDestPureRank());
}

char* ProcessChannel::GetDescription() {
    sprintf(description_, "\t[pc %p]\tr%d -> r%d", this,
            send_channel_->GetSenderPureRank(),
            recv_channel_->GetDestPureRank());
    return description_;
};

#endif

// } // namespace process_channel_v4
