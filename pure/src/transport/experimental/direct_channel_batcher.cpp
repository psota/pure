// Author: James Psota
// File:   direct_channel_batcher.cpp 

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

#include <atomic>
#include <cstring>
#include <stdint.h>

#include "pure.h"
#include "pure/transport/experimental/direct_channel_batcher.h"
#include "pure/transport/mpi_pure_helpers.h"

using namespace PureRT;

// debugging flags
#define DO_MPI_CALLS 1
#define PURE_RANK \
    (pure_thread_global == nullptr ? -1 : pure_thread_global->Get_rank())
#define BATCHER_DEBUG_PRINT_DEQUEUE 1

#define MPI_NULL_REQUEST -1
#define MPI_RESERVED_REQUEST -333
#define RANK_LEADER_NULL -1
#define BYTES_PROMISED_IRECV_LEADER_CLAIM -1

// TODO: ask for the size!
DirectChannelBatcher::DirectChannelBatcher(
        int mate_mpi_rank_arg, PureRT::EndpointType endpoint_type_arg,
        int threads_per_process, bool is_first_batcher)
    : mate_mpi_rank(mate_mpi_rank_arg), num_threads(threads_per_process),
      endpoint_type(endpoint_type_arg),
      thread_completion_trackers_total_bytes(
              sizeof(DestThreadMessageCompletionTracker) *
              threads_per_process) {

    batch_buf = jp_memory_alloc<1, CACHE_LINE_BYTES, true>(
            send_bytes_threshold * send_bytes_buf_factor, &send_bytes_max);

    wait_leader.store(RANK_LEADER_NULL);

    if (endpoint_type == PureRT::EndpointType::SENDER) {
        // must come last ////////////////////////////////////////////
        if (is_first_batcher) {
            // the other ones get set upon first use by the driver in
            // SendChannel
            ResetEnqueue(true);
        }
    } else if (endpoint_type == PureRT::EndpointType::RECEIVER) {
        thread_completion_trackers =
                new DestThreadMessageCompletionTracker[threads_per_process];

        active_threads.store(0);

        if (is_first_batcher) {
            DoDequeueMpiIrecv();
        }

        // normally this is done during the DequeueWait but for the first time
        // we need to do it here. It's no longer done in ResetDequeue.
        bytes_committed_or_consumed.store(0);
        ResetDequeue();
        AllowDequeueEntry();
    } else {
        sentinel("Invalid endpoint type");
    }
}

DirectChannelBatcher::~DirectChannelBatcher() {
    if (endpoint_type == PureRT::EndpointType::RECEIVER) {
        if (bytes_promised.load() == 0) {
            // presumably this was never used, so cancel the speculative request
            // that was issued by RecvChannel
            CancelOutstandingRequest();
        }
        delete[](thread_completion_trackers);
    }
    free(batch_buf);
}

/*
 * This is called by a single thread when it is done with everything (right
 * before returning from orig_main) to finish up any straggler messages. Right
 * now it's implemented for send only.
 */
void DirectChannelBatcher::Finalize() {
    if (endpoint_type == PureRT::EndpointType::SENDER) {
        const auto bytes_committed_or_consumed_local =
                bytes_committed_or_consumed.load();
        if (bytes_committed_or_consumed_local > 0 &&
            bytes_committed_or_consumed_local < send_bytes_threshold) {
            // there's more to send -- the last batch
            fprintf(stderr,
                    KYEL "DCB Finalize --- sending last batch which is %d "
                         "bytes\n" KRESET,
                    bytes_committed_or_consumed_local);
            DoEnqueueMPISend(bytes_committed_or_consumed_local);
        }
    }
}

void DirectChannelBatcher::CancelOutstandingRequest() {
    MPI_Request temp_req = outstanding_req.load();

    // consider using MPI_Request_get_status ??
    if (BATCHER_DEBUG_PRINT) {
        fprintf(stderr, " dcon: trying to cancel request 0x%x...\n", temp_req);
    }
    int ret = MPI_Cancel(&temp_req);
    assert(ret == MPI_SUCCESS);
    ret = MPI_Request_free(&temp_req);
    assert(ret == MPI_SUCCESS);
}

/////////////////////////////////////////////////////////////////////////

/*
   Enqueue a single message into the master buffer, and possibly send it if
   a send criterion is met.
*/
bool DirectChannelBatcher::EnqueueSingleMsg(void*        msg_payload,
                                            int          payload_bytes,
                                            int          sender_pure_rank,
                                            int          receiver_pure_rank,
                                            MPI_Datatype datatype, int tag,
                                            bool& became_send_leader) {

    assert(payload_bytes + sizeof(BatcherHeader) <= send_bytes_threshold);
    became_send_leader = false; // default

    // 0. wait until there's space in the buffer. when there is, get the
    // offset where I will add my message into the buffer
    int curr_bytes_promised, new_bytes_promised;

    // comment deprecated
    // our goal is to have exactly one thread that overruns the
    // send_bytes_threshold. We do that by "straddling" the
    // send_bytes_threshold -- before the CAS the current value needs to be
    // less than the threshold and the new value needs to be more than it.
    // Only if it succeeds does the enqueue succeed.
    curr_bytes_promised = bytes_promised.load();
    if (curr_bytes_promised > send_bytes_threshold) {
        return false;
    }

    assert(payload_bytes > 0);
    new_bytes_promised =
            curr_bytes_promised + sizeof(BatcherHeader) + payload_bytes;
    assert(new_bytes_promised > curr_bytes_promised);
    const int new_bytes_promised_attempted = new_bytes_promised;

    // ok, it's possible, so try to update it. Note that this will only succeed
    // when curr_bytes_promised is what we want (a "low" value, below the
    // threshold). Otherwise we would have returned above.
    const auto update_succeeded = bytes_promised.compare_exchange_strong(
            curr_bytes_promised, new_bytes_promised);

    if (update_succeeded) {
        if (BATCHER_DEBUG_PRINT)
            fprintf(stderr,
                    KGRN "r%d %p \tbytes_promised: from %d --> %d (payload: "
                         "%u)\n" KRESET,
                    PURE_RANK, this, curr_bytes_promised, new_bytes_promised,
                    static_cast<int*>(msg_payload)[0]);
    } else {
        if (BATCHER_DEBUG_PRINT)
            fprintf(stderr,
                    KRED
                    "r%d %p\ttried to do bytes_promised: from %d -> %d, but "
                    "UPDATE FAILED "
                    "(payload: %u)\n" KRESET,
                    PURE_RANK, this, curr_bytes_promised,
                    new_bytes_promised_attempted,
                    static_cast<int*>(msg_payload)[0]);
        // it's not possible to enqueue now, as the update failed which means
        // the buffer is already full
        return false;
    }

    assert(update_succeeded);
    // now, make sure see if we didn't overrun the buffer. If we did,
    // right now we just fail.
    check(new_bytes_promised <= send_bytes_max,
          "new bytes promised %d is now > send bytes max (%d)",
          new_bytes_promised, send_bytes_max);
// there is room in the buffer for my message. yay.
#if ENABLE_DCB_TIMEOUT
    if (curr_bytes_promised == 0) {
        // first message in so mark the time
        cycle_of_first_message_enqueued.store(rdtscp());
    }
#endif

    // 1. now we have space in the buffer, so write it. Every thread can do
    // this in parallel.

    // 1a. write the header
    const BatcherHeader header{payload_bytes, sender_pure_rank,
                               receiver_pure_rank, datatype, tag};
    void* const         header_buf =
            static_cast<char*>(batch_buf) + curr_bytes_promised;
    std::memcpy(header_buf, &header, sizeof(BatcherHeader));

    // 1b. write the payload
    void* const payload_buf =
            static_cast<char*>(header_buf) + sizeof(BatcherHeader);
    std::memcpy(payload_buf, msg_payload, payload_bytes);

    // 2. Mark that I'm done my copying into the batch buf
    bytes_committed_or_consumed += sizeof(BatcherHeader) + payload_bytes;

    // 3. now, check to see if we should be the thread to actually send this
    // message
    became_send_leader = SendIfPossible();

    if (became_send_leader) {
        if (BATCHER_DEBUG_PRINT)
            fprintf(stderr,
                    KGRN "r%d  %p DID become send leader from end of "
                         "EnqueueSingle\n" KRESET,
                    PURE_RANK, this);
    } else {
        if (BATCHER_DEBUG_PRINT)
            fprintf(stderr,
                    "r%d  %p did NOT become send leader from end of "
                    "EnqueueSingle \n",
                    PURE_RANK, this);
    }

    // did add my message to the batch
    return true;
}

// new_bytes_promised is passed in from EnqueueSingleMessage to save a cache
// miss as we already have that value.
bool DirectChannelBatcher::SendIfPossible() {
    if (ReadyToSend()) {
        // TODO rename variable if this works
        auto       rank_leader_null_lval = RANK_LEADER_NULL;
        const auto am_leader             = wait_leader.compare_exchange_strong(
                rank_leader_null_lval, PURE_RANK);

        if (am_leader) {
            DoEnqueueMPISend(bytes_committed_or_consumed.load());
            return true;
        } else {
            if (BATCHER_DEBUG_PRINT)
                fprintf(stderr,
                        "r%d -- message was ready to send, but I did NOT "
                        "become "
                        "enqueue send leader and not "
                        "sending\n",
                        PURE_RANK);
        }
    }

    // otherwise, message is not ready to send or it is but I didn't become
    // the leader. fallthrough for all other cases.
    return false;
}

void DirectChannelBatcher::DoEnqueueMPISend(int bytes_to_send) const {
    assert(bytes_to_send <= send_bytes_max);
    assert(bytes_to_send == bytes_committed_or_consumed.load());
    const auto ret = MPI_Send(batch_buf, bytes_to_send, MPI_BYTE, mate_mpi_rank,
                              batch_tag, MPI_COMM_WORLD);
    assert(ret == MPI_SUCCESS);
    PrintBatchedMessage(this, batch_buf, bytes_to_send, true);
}

bool DirectChannelBatcher::ReadyToSend() const {
    // the first condition is local to the callers cache and should act as
    // an early exit condition for some cases. The second condition makes
    // sure all promised bytes are actually committed (into the batch
    // buffer).
    const auto bytes_committed_or_consumed_local =
            bytes_committed_or_consumed.load();
    const auto bytes_promised_local = bytes_promised.load();

    check(bytes_committed_or_consumed_local <= bytes_promised_local,
          "bytes_committed is %d and expected it to be less than or equal to "
          "bytes_promised_local %d",
          bytes_committed_or_consumed_local, bytes_promised_local);

    // we want to send simply when the committed amount is both above the
    // threshold AND equal to the promised amount
    const bool send_due_to_batch_size_and_completedness =
            (bytes_committed_or_consumed_local > send_bytes_threshold &&
             bytes_committed_or_consumed_local == bytes_promised_local);

    if (BATCHER_DEBUG_PRINT)
        fprintf(stderr,
                KYEL "====  r%d bytes_committed_or_consumed_local %d   "
                     "send_bytes_threshold %d  "
                     " bytes_promised (refreshed) %d\n" KRESET,
                PURE_RANK, bytes_committed_or_consumed_local,
                send_bytes_threshold, bytes_promised.load());

    return send_due_to_batch_size_and_completedness ||
           SendDueToTimeout(bytes_committed_or_consumed_local);
}

#define ENABLE_DCB_TIMEOUT 0
bool DirectChannelBatcher::SendDueToTimeout(int bytes_to_send) const {
#if ENABLE_DCB_TIMEOUT
    // exit early for special case
    if (bytes_to_send == 0) {
        return false;
    }

    const auto timed_out = (rdtscp() - cycle_of_first_message_enqueued.load() >=
                            send_occupancy_cycles_threshold);
    if (timed_out) {
        if (BATCHER_DEBUG_PRINT)
            fprintf(stderr,
                    KRED_BG
                    "r%d SendDueToTimeout TIMED OUT!!!! Sending whatever I "
                    "have now.\n" KRESET,
                    PURE_RANK);
    }
    return timed_out;
#else
    return false;
#endif
}

void DirectChannelBatcher::ResetEnqueue(bool calling_from_constructor) {
    // outstanding_req.store(MPI_NULL_REQUEST);
    if (BATCHER_DEBUG_PRINT)
        fprintf(stderr,
                KRED_BG "r%d DCB %p RESET ENQUEUE RESET ENQUEUE RESET ENQUEUE "
                        "\n" KRESET,
                PURE_RANK, this);

    // remove this for now
    if (false && calling_from_constructor == false) {
        // this check doesn't work upon initiat reset (in constructor)
        const auto bytes_committed_local = bytes_committed_or_consumed.load();
        const auto bytes_promised_local  = bytes_promised.load();

        if (bytes_committed_local != bytes_promised_local) {
            print_pure_backtrace();
        }
        check(bytes_committed_local == bytes_promised_local,
              "r%d expected these to be the same upon reset enqueue -- "
              "bytes_committed_or_consumed = %d   bytes_promised = %d",
              PURE_RANK, bytes_committed_local, bytes_promised_local);

        // on the first use for all of them, bytes_committed will be 0. This
        // could be made to work but I don't see evidence of an issue right now
        // so I'm removing the check. check(bytes_committed_local >
        // send_bytes_threshold,
        //       "expected bytes_committed_local (%d) to be greater than the
        //       send " "threshold (%d) but it wasn't", bytes_committed_local,
        //       send_bytes_threshold);
    }

    // TODO: reset these all with one instruction.

    wait_leader.store(RANK_LEADER_NULL);

    // store zero to both counters
    // this is apparently undefined behavior as per stack overflow. there may be
    // a clean way to do this with variants with one type that is two ints and
    // another that is one int64_t static_assert(sizeof(atomic<int>) == 4);
    // assert_cacheline_aligned(&bytes_promised);
    // atomic<int64_t>* const counter_ptr =
    //         reinterpret_cast<atomic<int64_t>*>(&bytes_promised);
    // counter_ptr->store(0);

    // note: this may not actually work given that they can be written
    // very quickly
    // assert(bytes_committed_or_consumed.load() == 0);
    // assert(bytes_promised.load() == 0);

    // OPT: potential optimization or correctness optimizatoin -- reset both of
    // these variables at the same time. They would have to go on the same cache
    // line. Then they could just be zeroed out with a single atomic write.
    bytes_committed_or_consumed.store(0);

    // THIS MUST BE LAST!
    // must come last ////////////////////////////////////////////
    // must come last ////////////////////////////////////////////
    // must come last ////////////////////////////////////////////
    bytes_promised.store(0);
}

// static
void DirectChannelBatcher::PrintBatchedMessage(
        DirectChannelBatcher const* const ptr, void* batch_buf, int total_bytes,
        bool is_sender) {
#if BATCHER_DEBUG_PRINT
    // iterate through the message, piece by piece
    // assume int payload for now -- templatize later
    void* curr_buf        = batch_buf;
    int   bytes_processed = 0;

    if (is_sender) {
        fprintf(stderr,
                KGREY "[r%d] %p BATCHED MESSAGE FROM SENDER "
                      "---------------->\n" KRESET,
                PURE_RANK, ptr);
    } else {
        fprintf(stderr,
                KGREY KUNDER "[r%d] %p ----------------> BATCHED MESSAGE ON "
                             "RECEIVER\n" KRESET,
                PURE_RANK, ptr);
    }

    while (bytes_processed < total_bytes) {
        BatcherHeader* h = static_cast<BatcherHeader*>(curr_buf);
        check(h->start_word == BATCHER_HEADER_NONCE,
              "expected nonce, but is %d",
              h->start_word); // debug mode only

        if (is_sender) {
            fprintf(stderr,
                    KYEL "  [r%d] [ %dB  r%d -> r%d  d:%d  tag:%d \t| " KRESET,
                    PURE_RANK, h->payload_bytes, h->sender_pure_rank,
                    h->receiver_pure_rank, static_cast<int>(h->datatype),
                    h->tag);
        } else {
            fprintf(stderr,
                    KYEL KUNDER
                    "       [r%d] [ %dB  r%d -> r%d  d:%d  tag:%d \t| " KRESET,
                    PURE_RANK, h->payload_bytes, h->sender_pure_rank,
                    h->receiver_pure_rank, static_cast<int>(h->datatype),
                    h->tag);
        }

        curr_buf = static_cast<char*>(curr_buf) + sizeof(BatcherHeader);
        bytes_processed += sizeof(BatcherHeader);

        const auto num_ints = h->payload_bytes / sizeof(int);
        for (auto p = 0; p < num_ints; ++p) {
            fprintf(stderr, "%d:%u ", p, static_cast<int*>(curr_buf)[p]);
        }
        fprintf(stderr, "]\n");

        curr_buf = static_cast<char*>(curr_buf) + h->payload_bytes;
        bytes_processed += h->payload_bytes;
        assert(bytes_processed <= total_bytes);
    }
#endif
}

///////////////////////////////////////////////////////////////
/*
 * This function is called concurrently by all threads that *may* have a message
 in this batch.

 Here are the basic steps:
 1. If the message hasn't been "waited" on yet, do so

 2. Look through the batch for my desired message. If you find the message, mark
 those bytes as consumed globally and also mark as "completed" locally in my
 message completion tracker.

 3. Threads that notice that all bytes have been consumed should try to become
 the recv leader and proactively do the next recv.

 */
bool DirectChannelBatcher::DequeueWaitAndTryToConsumeSingleMessage(
        void* const dest_buf, int payload_max_bytes, int sender_pure_rank,
        int receiver_pure_rank, MPI_Datatype datatype, int tag,
        int thread_num_in_proc, bool& became_irecv_leader) {

    ThreadEnteredDequeue();

    assert(receiver_pure_rank == PURE_RANK);
    bool found_message  = false;
    became_irecv_leader = false; // default

    if (BATCHER_DEBUG_PRINT && BATCHER_DEBUG_PRINT_DEQUEUE)
        fprintf(stderr,
                "-- r%d starting DequeueWaitAndTryToConsumeSingleMessage "
                "(%p)\n",
                PURE_RANK, this);

    // 0. Early exit if all the messages here are done being consumed already
    int bytes_promised_local;
    // OPT -- is this check necessary?
    // if (AllDequeueMsgsConsumed(bytes_promised_local)) {
    //     ThreadExitedDequeue();
    //     return false;
    // }

    //////////////////////////////////////////////////////////////////////////
    // 1. if MPI_Wait hasn't been issued yet, do so. Elect a leader. First one
    // in gets to call MPI_Wait right away as we know that the dequeue was
    // already issued.

    do {
        if (wait_leader.load() == RANK_LEADER_NULL) {
            auto wait_leader_curr_val = RANK_LEADER_NULL;

            // OPT: change this to "weak"
            const auto am_wait_leader = wait_leader.compare_exchange_strong(
                    wait_leader_curr_val, PURE_RANK);

            if (am_wait_leader) {
                // Do the wait and update some variables
                MPI_Status  status;
                MPI_Request tmp_req = outstanding_req.load();
                if (BATCHER_DEBUG_PRINT && BATCHER_DEBUG_PRINT_DEQUEUE)
                    fprintf(stderr,
                            "r%d -- I am Dequeue Wait leader. About to do MPI "
                            "WAIT "
                            "on req %d "
                            "------------------\n",
                            PURE_RANK, tmp_req);
                PureRT::do_mpi_wait_with_work_steal(pure_thread_global,
                                                    &tmp_req, &status);
                int       bytes_received_count;
                const int count_ret =
                        MPI_Get_count(&status, MPI_BYTE, &bytes_received_count);
                assert(count_ret == MPI_SUCCESS);
                assert(bytes_received_count <= send_bytes_max);
                if (BATCHER_DEBUG_PRINT && BATCHER_DEBUG_PRINT_DEQUEUE)
                    fprintf(stderr,
                            "r%d -- Finished MPI WAIT and got %d bytes "
                            "------------------\n",
                            PURE_RANK, bytes_received_count);
                PrintBatchedMessage(this, batch_buf, bytes_received_count,
                                    false);
                // save how many bytes actually arrived, which is useful below
                // and also releases the other threads from this loop.
                bytes_promised.store(bytes_received_count);
            }
        }
        bytes_promised_local = bytes_promised.load();
    } while (bytes_promised_local == 0);

    assert(bytes_promised_local > 0);
    //////////////////////////////////////////////////////////////////////////

    // TODO possibly return early if this has already been fully consumed.

    // 2. try to consume my desired message, which may not be in this bundle
    void* curr_buf = batch_buf; // start at the beginning
    // for receiver, bytes_promised means bytes_in_message
    int bytes_left_to_scan = bytes_promised_local;

    // for use with the DestThreadMessageCompletionTracker
    int                                 num_msgs_checked = 0;
    DestThreadMessageCompletionTracker& completion_tracker =
            GetCompletionTracker(thread_num_in_proc);

    // TODO? possibly optimize the scan by jumping ahead using the
    // completion info

    while (bytes_left_to_scan > 0) {
        // fprintf(stderr,
        //         " >>>> r%d -- trying to scan %d bytes to find my message\n",
        //         PURE_RANK, bytes_left_to_scan);
        BatcherHeader* h = static_cast<BatcherHeader*>(curr_buf);
#if DEBUG_CHECK
        check(h->start_word == BATCHER_HEADER_NONCE,
              "r%d\texpected nonce, but is %d", PURE_RANK,
              h->start_word); // debug mode only

        // fprintf(stderr,
        //         KCYN "r%d:\tRECV  [ %dB  r%d -> r%d  d:%d  tag:%d ]\n"
        //         KRESET, PURE_RANK, h->payload_bytes, h->sender_pure_rank,
        //         h->receiver_pure_rank, static_cast<int>(h->datatype),
        //         h->tag);
#endif

        const auto bytes_this_message =
                (sizeof(BatcherHeader) + h->payload_bytes);

        /// I think this comment is deprecated
        // there is a race here, although it may be innocuous.
        // completion_tracker will get clobered (all set to "false") during
        // ResetDequeue. This thread can still be looping in here, looking at
        // each message when another thread resets. The non-innocuous problem is
        // when an actual irecv receives data and overrwrites batch_buf
        // with new data. Now this thread is in this loop, looking for new
        // messages. It could have a non-zero bytes_left_to_scan and
        // batch_buf could be swapped out. This is a problem for a number
        // of reasons --- the irecv is certainly non-atomic so the message
        // contents could be undefined behavior. Also, num_msgs_checked could be
        // non-zero as well so a later message could be checked in the buffer,
        // meaning there could be out-of-order receives.

        const bool msg_already_consumed =
                completion_tracker.IsMsgNumCompleted(num_msgs_checked);
        // fprintf(stderr, "        r%d msg: %d msg_already_consumed = %d\n",
        //         PURE_RANK, num_msgs_checked, msg_already_consumed);

        // regardless of it being a match or not, we mark this as completed
        // it's not actually complete as of this point in the program, but
        // the match case includes an early exit so we mark it as complete
        // here.
        if (msg_already_consumed == false &&
            h->sender_pure_rank == sender_pure_rank &&
            h->receiver_pure_rank == receiver_pure_rank &&
            h->datatype == datatype && h->tag == tag) {
            // conservative check for now
            assert(h->payload_bytes == payload_max_bytes);
            assert(h->receiver_pure_rank == PURE_RANK);

            // there's a match! consume the message by placing it into my
            // desired destination buffer.
            void* const src_buf =
                    static_cast<char*>(curr_buf) + sizeof(BatcherHeader);
            std::memcpy(dest_buf, src_buf, payload_max_bytes);

            // complete as I consumed it
            completion_tracker.MarkMsgNumComplete(num_msgs_checked);
            found_message = true;

            if (BATCHER_DEBUG_PRINT && BATCHER_DEBUG_PRINT_DEQUEUE) {
                fprintf(stderr,
                        KGRN
                        "\t\t  r%d  message matched! %d     %d -> %d  tag: "
                        "%d\n" KRESET,
                        PURE_RANK, static_cast<int*>(src_buf)[0],
                        h->sender_pure_rank, h->receiver_pure_rank, h->tag);
            }

            // this is the line that potentially allows another thread to
            // proceed with the next irecv, which could overrwrite
            // batch_buf
            bytes_committed_or_consumed += bytes_this_message;
            break; // found desired message
        } else {
            if (msg_already_consumed == false && BATCHER_DEBUG_PRINT &&
                BATCHER_DEBUG_PRINT_DEQUEUE) {
                void* const src_buf =
                        static_cast<char*>(curr_buf) + sizeof(BatcherHeader);
                fprintf(stderr,
                        "\t\t  r%d  message did NOT match %d -> %d  "
                        "(wanted %d "
                        "-> %d) dt: %d (wanted %d)  tag: %d (wanted "
                        "%d) payload: %u  num_msgs_checked: %d\n",
                        PURE_RANK, h->sender_pure_rank, h->receiver_pure_rank,
                        sender_pure_rank, receiver_pure_rank, h->datatype,
                        datatype, h->tag, tag, static_cast<int*>(src_buf)[0],
                        num_msgs_checked);
            }
        }

        // it can also be complete if it's for another destination rank

        ///////////////////////
        /// this is an optimzation -- remove this for now to see if this is
        /// an issue
#if 0
        if (h->receiver_pure_rank != receiver_pure_rank) {
            // it will never match (e.g., with a different tag,
            // etc.) so mark as complete
            completion_tracker.MarkMsgNumComplete(num_msgs_checked);
        }
#endif
        curr_buf = static_cast<char*>(curr_buf) + bytes_this_message;
        bytes_left_to_scan -= bytes_this_message;
        assert(bytes_left_to_scan >= 0);
        ++num_msgs_checked;
        assert(num_msgs_checked <= max_messages_per_batch);
    } // ends while not finished scanning all bytes

    //////////////////////////////////////////////////////////////////////////
    // 3. When all messages in the batch_buf have been consumed, issue the
    // next irecv and reset these variables

    // how do we decide the leader for the next irecv? we really just want the
    // first thread here that notices that all bytes have been consumed. That
    // guy gets to be the leader.

    // new
    // here we try to claim comm leadership in a way that we were already
    // planning to do -- which is to get started on the actual dequeue reset.
    int        bytes_committed_or_consumed_local;
    const auto all_deq_msgs_consumed =
            AllDequeueMsgsConsumed(bytes_committed_or_consumed_local);
    if (all_deq_msgs_consumed) {
        // try to reset bytes_consumed, which we know to be above zero at this
        // point set the output parameter
        became_irecv_leader =
                bytes_committed_or_consumed.compare_exchange_strong(
                        bytes_committed_or_consumed_local, 0);

        if (became_irecv_leader) {
            if (BATCHER_DEBUG_PRINT && BATCHER_DEBUG_PRINT_DEQUEUE)
                fprintf(stderr, "r%d r consumed %d -> 0 found_message is %d\n",
                        PURE_RANK, bytes_committed_or_consumed_local,
                        found_message);
        }

        if (became_irecv_leader && BATCHER_DEBUG_PRINT &&
            BATCHER_DEBUG_PRINT_DEQUEUE) {
            fprintf(stderr,
                    KBOLD "r%d Became the next irecv leader (from DCB "
                          "%p).\n" KRESET,
                    PURE_RANK, this);
        }
    }

    if (BATCHER_DEBUG_PRINT && BATCHER_DEBUG_PRINT_DEQUEUE) {
        if (found_message) {
            fprintf(stderr,
                    KGRN "         r%d  LEAVING DeqeuueWait -- FOUND "
                         "MESSAGE\n" KRESET,
                    PURE_RANK);
        } else {
            fprintf(stderr,
                    KRED "         r%d  LEAVING DeqeuueWait -- DID NOT FIND "
                         "MESSAGE\n" KRESET,
                    PURE_RANK);
        }
    }

    ThreadExitedDequeue();
    return found_message;
}

DestThreadMessageCompletionTracker&
DirectChannelBatcher::GetCompletionTracker(int thread_num_in_proc) {
    return thread_completion_trackers[thread_num_in_proc];
}

bool DirectChannelBatcher::AllDequeueMsgsConsumed(
        int& bytes_committed_or_consumed_local) const {

    // just to save a fetch by the caller
    bytes_committed_or_consumed_local = bytes_committed_or_consumed.load();
    const auto bytes_promised_local   = bytes_promised.load();

    // bytes_promised may have been reset to zero, so we only do this check
    // if it didn't drop to zero. bytes_promised gets cleared first in
    // ResetDequeue, so if that's zero, we know we can't be the comm_leader
    const auto all_deq_msgs_consumed =
            (bytes_promised_local > 0 &&
             bytes_committed_or_consumed_local == bytes_promised_local);
    return all_deq_msgs_consumed;
}

void DirectChannelBatcher::DoDequeueMpiIrecv() {
    MPI_Request temp_req;
    const auto  ret =
            MPI_Irecv(batch_buf, send_bytes_max, MPI_BYTE, mate_mpi_rank,
                      batch_tag, MPI_COMM_WORLD, &temp_req);
    assert(ret == MPI_SUCCESS);
    outstanding_req.store(temp_req);
    if (BATCHER_DEBUG_PRINT && BATCHER_DEBUG_PRINT_DEQUEUE)
        fprintf(stderr, KYEL "r%d  did MPI_Irecv and req is now 0x%x\n" KRESET,
                PURE_RANK, temp_req);
}

void DirectChannelBatcher::ThreadEnteredDequeue() {
    assert(active_threads.load() >= 0);
    // fprintf(stderr, "r%d - increasing active threads\n", PURE_RANK);
    ++active_threads;
    // fprintf(stderr, "r%d - DONE increasing active threads\n", PURE_RANK);
    assert(active_threads.load() <= num_threads);
}

void DirectChannelBatcher::ThreadExitedDequeue() {
    assert(active_threads.load() >= 1);
    // fprintf(stderr, "r%d - decreasing active threads\n", PURE_RANK);
    --active_threads;
    // fprintf(stderr, "r%d - DONE decreasing active threads\n", PURE_RANK);
    assert(active_threads.load() < num_threads);
    assert(active_threads.load() >= 0);
}

void DirectChannelBatcher::AllowDequeueEntry() { entry_allowed.store(true); }

void DirectChannelBatcher::DisallowDequeueEntry() {
    entry_allowed.store(false);
}

bool DirectChannelBatcher::DequeueEntryAllowed() const {
    return entry_allowed.load() == true;
}

bool DirectChannelBatcher::DequeueEntryNotAllowed() const {
    return entry_allowed.load() == false;
}

void DirectChannelBatcher::WaitUntilDequeueEntryAllowed() const {
    while (DequeueEntryNotAllowed()) {
        x86_pause_instruction();
    }
}

void DirectChannelBatcher::WaitUntilNoActiveDequeueThreads() const {
    int active_threads_local;
    while ((active_threads_local = active_threads.load()) > 0) {
        x86_pause_instruction();
    }
    assert(active_threads_local == 0);
}

void DirectChannelBatcher::ResetDequeue() {
    assert(active_threads.load() == 0);

    // note that bytes_committed_or_consumed should be set before this point,
    // either in the constructor or at the END of a DequeueWait
    assert(bytes_committed_or_consumed.load() == 0);

    // this must come first!
    bytes_promised.store(0);
    /////////////////////
    /////////////////////

    // there is a known race condition with this memset, which we hope is not a
    // practical issue, but it remains to be seen. Basically a thread can be
    // scanning a batch buf for a match and this can get cleared. Then it could
    // match a message that has the same header as another header, but one that
    // it already received, thereby receiving the same message twice. This would
    // be an error. We hope to practically avoid this given the array of
    // DirectChannelBatchers that are used via the BatcherManager driver.

    // TODO: clear this out with one memset call?
    // for (auto i = 0; i < threads_per_process; ++i) {
    //     thread_completion_trackers[i].MarkAllNotComplete();
    // }
    std::memset(thread_completion_trackers, 0,
                thread_completion_trackers_total_bytes);

    // importantly, comm_leader gets reset by the wait leader. This has to
    // occur in this order, otherwise if this is reset here, multiple
    // comm_leaders can be elected in quick succession.
    // comm_leader.store(RANK_LEADER_NULL);
    ////////////////////
    ////////////////////
    ////////////////////
    // must come last
    wait_leader.store(RANK_LEADER_NULL);
}