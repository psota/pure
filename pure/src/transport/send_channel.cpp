// Author: James Psota
// File:   send_channel.cpp 

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

#include "pure/transport/send_channel.h"
#include "pure/runtime/batcher_manager.h"
#include "pure/runtime/pure_thread.h"
#include "pure/transport/bundle_channel.h"
#include "pure/transport/direct_mpi_channel.h"
#include "pure/transport/experimental/direct_channel_batcher.h"
#include "pure/transport/experimental/send_flat_batcher.h"
#include "pure/transport/process_channel.h"
#include "pure/transport/process_channel_v4.h"
#include "pure/transport/rdma_mpi_channel.h"
#include "pure/transport/send_bundle_channel.h"
#include "pure/transport/sender_channel_state.h"

SendChannel::SendChannel(void* const send_buf_arg, int count_arg,
                         MPI_Datatype datatype_arg, int sender_pure_rank_arg,
                         int tag_arg, int dest_pure_rank_arg,
                         channel_endpoint_tag_t channel_endpoint_tag_arg,
                         ofstream&   pure_thread_trace_comm_outstream_arg,
                         bool        using_rt_send_buf_hint,
                         PureThread* pure_thread_arg)
    : BundleChannelEndpoint(send_buf_arg, nullptr, count_arg, datatype_arg,
                            sender_pure_rank_arg, tag_arg, dest_pure_rank_arg,
                            channel_endpoint_tag_arg, pure_thread_arg),
      pure_thread_trace_comm_outstream(pure_thread_trace_comm_outstream_arg),
      using_rt_send_buf_hint(using_rt_send_buf_hint) {
    // AssertNoSelfSend();
}

SendChannel::~SendChannel() = default;

void SendChannel::SetInitializedState() {
#if DEBUG_CHECK
    sender_state.JustDidInitialize();
#endif
}
int SendChannel::GetChannelState() const {
#if DEBUG_CHECK
    return sender_state.ToInt();
#else
    return -1;
#endif
}

void SendChannel::AssertNoSelfSend() {
    check(!IsSendToSelfChannel(),
          "sender pure rank (%d) must be different from dest pure rank "
          "(%d) as this is not yet implemented.",
          sender_pure_rank, dest_pure_rank);
}

void SendChannel::Enqueue(size_t count_to_send) {
    assert(count_to_send <= count);
    if (count_to_send == UNSPECIFIED_COUNT_SENTINEL) {
        count_to_send = count;
    }

    switch (channel_type) {
    case PureRT::ChannelEndpointType::DIRECT_MPI:
        direct_mpi_channel->Enqueue(count_to_send);
        EnqueuePrologue(true);
        break;
    case PureRT::ChannelEndpointType::PROCESS:
        process_channel->Enqueue(count_to_send);
        EnqueuePrologue(false);
        break;
    case PureRT::ChannelEndpointType::SEND_TO_SELF:
        sentinel("Send to self is not implemented for enqueue runtime buffer. "
                 "use a user buffer.");
        break;
    case PureRT::ChannelEndpointType::FLAT_BATCHER:
        sentinel("Flat batcher not implemented for enqueue runtime buffer. "
                 "use a user buffer.");
        break;
    case PureRT::ChannelEndpointType::DIRECT_BATCHER:
        sentinel("Direct batcher not implemented for enqueue runtime buffer. "
                 "use a user buffer.");
        break;
    case PureRT::ChannelEndpointType::RDMA_MPI:
        rdma_mpi_channel->Enqueue(nullptr, count_to_send);
        EnqueuePrologue(true);
        break;
    default:
        sentinel("Invalid channel type %d", channel_type);
    }

#if DEBUG_CHECK
    sender_state.JustDidEnqueue();
#endif
}

void SendChannel::EnqueueUserBuf(const buffer_t user_buf,
                                 size_t         count_to_send) {
    check(count_to_send <= count,
          "count_to_send: %u   count (used to initialize the channel): %u",
          count_to_send, count);
    if (count_to_send == UNSPECIFIED_COUNT_SENTINEL) {
        count_to_send = count;
    }

#if DEBUG_CHECK
    if (using_rt_send_buf_hint == true) {
        fprintf(stderr, "Warning: calling EnqueueUserBuf but "
                        "using_rt_send_buf_hint is true. Since you are not "
                        "using the runtime buffer, it is recommended to set "
                        "using_rt_send_buf_hint to false as an optimization.");
    }
#endif

    switch (channel_type) {
    case PureRT::ChannelEndpointType::DIRECT_MPI:
        direct_mpi_channel->EnqueueUserBuf(user_buf, count_to_send);
        EnqueuePrologue(true);
        break;
    case PureRT::ChannelEndpointType::PROCESS:
        process_channel->EnqueueUserBuf(user_buf, count_to_send);
        EnqueuePrologue(false);
        break;
    case PureRT::ChannelEndpointType::FLAT_BATCHER:
        FlatBatcherEnqueue(user_buf);
        EnqueuePrologue(true);
        break;
    case PureRT::ChannelEndpointType::DIRECT_BATCHER:
        // direct_channel_batcher->EnqueueSingleMsg(
        //         user_buf, NumPayloadBytes(), GetSenderPureRank(),
        //         GetDestPureRank(), GetDatatype(), GetTag());
        DirectBatcherEnqueue(user_buf);
        EnqueuePrologue(true);
        break;
    case PureRT::ChannelEndpointType::RDMA_MPI:
        sentinel("RDMA currendlyt doesn't support user bufs");
        break;
    case PureRT::ChannelEndpointType::SEND_TO_SELF:
        send_to_self_channel->EnqueueUserBuf(
                user_buf, count_to_send * GetDatatypeBytes());
        EnqueuePrologue(false);
        break;
    default:
        sentinel("Invalid channel type %d", channel_type);
    }

#if DEBUG_CHECK
    sender_state.JustDidEnqueue();
#endif
}

void SendChannel::FlatBatcherEnqueue(void* const user_buf) {
    flat_batcher->EnqueueMessage(pure_thread->GetThreadNumInProcess(), user_buf,
                                 NumPayloadBytes(), GetSenderPureRank(),
                                 GetDestPureRank(), GetDatatype(), GetTag());

    // put this channel on my local queue for the wait procedure
    this->ResetFlatBatcherWaitDone();
    pure_thread->EnqueueFlatBatcherIncompleteSendChan(this);
}

void SendChannel::FlatBatcherEnqueueWait() {
    while (this->FlatBatcherWaitIsDone() == false) {
        const auto num_enqueues_completed =
                pure_thread->FlatBatcherNumOutstandingEnqueues() -
                flat_batcher->NumPendingEnqueues(
                        pure_thread->GetThreadNumInProcess());
        assert(num_enqueues_completed >= 0);

        // go through these and mark them as done
        for (auto i = 0; i < num_enqueues_completed; ++i) {
            SendChannel* const sc =
                    pure_thread->DequeueFlatBatcherCompletedSendChan();
            sc->MarkFlatBatcherWaitDone();
        }
    }
}

void SendChannel::DirectBatcherEnqueue(void* user_buf) {

    // should I cache this???
    const auto receiver_mpi_rank =
            pure_thread->MPIRankFromPureRank(GetDestPureRank());
    auto batcher_manager =
            pure_thread->GetEnqueueBatcherManager(receiver_mpi_rank);
    bool       enqueue_succeeded, became_send_leader;
    const auto num_payload_bytes = NumPayloadBytes();

    do {
        DirectChannelBatcher* const curr_batcher =
                batcher_manager->GetCurrBatcher();

        enqueue_succeeded = curr_batcher->EnqueueSingleMsg(
                user_buf, num_payload_bytes, GetSenderPureRank(),
                GetDestPureRank(), GetDatatype(), GetTag(), became_send_leader);

        // if I became the irecv leader, then I have to kick off the next irecv.
        if (became_send_leader) {
            // 1. get the next batcher
            DirectChannelBatcher* const next_batcher =
                    batcher_manager->GetNextBatcher();

            // 2. reset the batcher
            if (BATCHER_DEBUG_PRINT) {
                fprintf(stderr,
                        KCYN " r%d RESETTING NEXT BATCHER from %p to %p to get "
                             "ready "
                             "for new "
                             "send.\n" KRESET,
                        pure_thread->Get_rank(),
                        batcher_manager->GetCurrBatcher(), next_batcher);
            }
            next_batcher->ResetEnqueue(false);

            // 3. update the current enqueue batcher so that all of the other
            // threads will use this new one going forward.
            batcher_manager->IncrementBatcher();
        }
    } while (enqueue_succeeded == false);
}

void SendChannel::Wait() {

    switch (channel_type) {
    case PureRT::ChannelEndpointType::DIRECT_MPI:
        direct_mpi_channel->EnqueueWait();
        break;
    case PureRT::ChannelEndpointType::FLAT_BATCHER:
        FlatBatcherEnqueueWait();
        break;
    case PureRT::ChannelEndpointType::DIRECT_BATCHER:
        // do nothing right now -- it already went out
        // direct_channel_batcher->EnqueueWait();
        break;
    case PureRT::ChannelEndpointType::PROCESS:
        process_channel->EnqueueWait();
        break;
    case PureRT::ChannelEndpointType::SEND_TO_SELF:
        // do nothing
        // send_to_self_channel->EnqueueWait();
        break;
    case PureRT::ChannelEndpointType::RDMA_MPI:
        rdma_mpi_channel->EnqueueWait();
        break;
    default:
        sentinel("Invalid channel type %d", channel_type);
    }

#if DEBUG_CHECK
    sender_state.JustDidLastWait();
#endif
}

bool SendChannel::Test() {

    bool       enqueue_test_succeeded = false;
    const auto do_blocking_wait       = false;

    switch (channel_type) {
    case PureRT::ChannelEndpointType::DIRECT_MPI:
        direct_mpi_channel->EnqueueWait(do_blocking_wait,
                                        &enqueue_test_succeeded);
        break;
    case PureRT::ChannelEndpointType::PROCESS:
        enqueue_test_succeeded = process_channel->EnqueueTest();
        break;
    default:
        sentinel("Invalid channel type %d", channel_type);
    }

#if DEBUG_CHECK
    if (enqueue_test_succeeded) {
        sender_state.JustDidLastWait();
    }
#endif

    return enqueue_test_succeeded;
}

void SendChannel::EnqueueBlocking(size_t count_to_send) {
    assert(count_to_send <= count);
    if (count_to_send == UNSPECIFIED_COUNT_SENTINEL) {
        count_to_send = count;
    }

    switch (channel_type) {
    case PureRT::ChannelEndpointType::DIRECT_MPI:
        direct_mpi_channel->EnqueueBlocking(count_to_send);
        EnqueuePrologue(true);
        break;
    case PureRT::ChannelEndpointType::PROCESS:
        process_channel->EnqueueBlocking(count_to_send);
        EnqueuePrologue(false);
        break;
    default:
        sentinel("Invalid channel type %d", channel_type);
    }
#if DEBUG_CHECK
    sender_state.JustDidBlockingEnqueue();
#endif
}

void SendChannel::EnqueueBlockingUserBuf(const buffer_t user_buf,
                                         size_t         count_to_send) {
    assert(count_to_send <= count);
    if (count_to_send == UNSPECIFIED_COUNT_SENTINEL) {
        count_to_send = count;
    }

    switch (channel_type) {
    case PureRT::ChannelEndpointType::DIRECT_MPI:
        direct_mpi_channel->EnqueueBlockingUserBuf(user_buf, count_to_send);
        EnqueuePrologue(true);
        break;
    case PureRT::ChannelEndpointType::PROCESS:
        process_channel->EnqueueBlockingUserBuf(user_buf, count_to_send);
        EnqueuePrologue(false);
        break;
    default:
        sentinel("Invalid channel type %d", channel_type);
    }
#if DEBUG_CHECK
    sender_state.JustDidBlockingEnqueue();
#endif
}

void SendChannel::EnqueuePrologue(bool mpi_transport) {
#if TRACE_COMM
    pure_thread_trace_comm_outstream
            << ", " /* file:line */
            << ", " /* function */
            << "send, " << GetSenderPureRank() << ", " << GetDestPureRank()
            << ", " << count << ", " << static_cast<unsigned int>(datatype)
            << ", " << tag << ", " << GetDatatypeBytes() << ", "
            << GetDatatypeBytes() * count << ", "
            << "Enqueue, " << GetChannelTypeName() << ", "
            << channel_endpoint_tag << ", "
            << (mpi_transport ? "MPI_YES" : "MPI_NO") << std::endl;
#endif
}
