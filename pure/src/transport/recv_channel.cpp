// Author: James Psota
// File:   recv_channel.cpp 

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

#include "pure/transport/recv_channel.h"
#include "pure/runtime/batcher_manager.h"
#include "pure/runtime/pure_thread.h"
#include "pure/transport/bundle_channel.h"
#include "pure/transport/bundle_channel_endpoint.h"
#include "pure/transport/direct_mpi_channel.h"
#include "pure/transport/experimental/direct_channel_batcher.h"
#include "pure/transport/experimental/recv_flat_batcher.h"
#include "pure/transport/process_channel.h"
#include "pure/transport/process_channel_v4.h"
#include "pure/transport/rdma_mpi_channel.h"
#include "pure/transport/recv_bundle_channel.h"
#include "pure/transport/recv_channel_state.h"
#include "pure/transport/send_to_self_channel.h"

RecvChannel::RecvChannel(void* const recv_buf_arg, int count_arg,
                         const MPI_Datatype& datatype_arg,
                         int sender_pure_rank_arg, int tag_arg,
                         int                    dest_pure_rank_arg,
                         channel_endpoint_tag_t channel_endpoint_tag_arg,
                         std::ofstream& pure_thread_trace_comm_outstream_arg,
                         PureRT::buffer_init_func_t init_func,
                         PureThread*                pure_thread_arg)

    : BundleChannelEndpoint(nullptr, recv_buf_arg, count_arg, datatype_arg,
                            sender_pure_rank_arg, tag_arg, dest_pure_rank_arg,
                            channel_endpoint_tag_arg, pure_thread_arg),

      pure_thread_trace_comm_outstream(pure_thread_trace_comm_outstream_arg),
      buf_init_func(init_func) {

    // if (sender_pure_rank_arg == dest_pure_rank_arg && DEBUG_CHECK) {
    //     print_pure_backtrace(stderr, '\n');
    // }
    // check(sender_pure_rank_arg != dest_pure_rank_arg,
    //       "sender pure rank (%d) must be different from dest pure rank "
    //       "(%d) as this is not yet implemented. Please implement if this "
    //       "feature is necessary",
    //       sender_pure_rank_arg, dest_pure_rank_arg);
}

RecvChannel::~RecvChannel() = default;

void RecvChannel::SetDequeueBatcherManager(BatcherManager* dbm) {
    assert(dequeue_batcher_manager == nullptr);
    dequeue_batcher_manager = dbm;
    channel_type            = PureRT::ChannelEndpointType::DIRECT_BATCHER;
}

void RecvChannel::SetInitializedState() {
#if DEBUG_CHECK
    receiver_state.JustDidInitialize();
#endif
}
void RecvChannel::SetWaitedState() {
#if DEBUG_CHECK
    ts_log_info("### RC::SetWaitedState: curr state: %d",
                receiver_state.ToInt());
    receiver_state.JustDidWait();
#endif
}
int RecvChannel::GetChannelState() const {
#if DEBUG_CHECK
    return receiver_state.ToInt();
#else
    return -1;
#endif
}

void RecvChannel::Dequeue(const buffer_t user_buf) {

    bool mpi_transport = false;

    switch (GetChannelType()) {
    case PureRT::ChannelEndpointType::DIRECT_MPI:
        direct_mpi_channel->Dequeue(user_buf);
        mpi_transport = true;
        break;
    case PureRT::ChannelEndpointType::FLAT_BATCHER:
#if FLAT_BATCHER_RECV_VERSION == 1
        assert(flat_batcher_msg_complete.load() == true);
        ResetFlatBatcherMsgCompleted();
        recv_flat_batcher->FindOrPostReceiveRequest(
                pure_thread->GetThreadNumInProcess(), this, user_buf,
                NumPayloadBytes(), GetSenderPureRank(), GetDestPureRank(),
                GetDatatype(), GetTag());
#endif
#if FLAT_BATCHER_RECV_VERSION == 2
        assert(flat_batcher_msg_complete == true);
        assert(flat_batcher_temp_user_recv_buf == nullptr);
        ResetFlatBatcherMsgCompleted();
        flat_batcher_temp_user_recv_buf = user_buf;

        recv_flat_batcher->PostReceiveRequest(
                pure_thread->GetThreadNumInProcess(), this,
                flat_batcher_temp_user_recv_buf, NumPayloadBytes(),
                GetSenderPureRank(), GetDestPureRank(), GetDatatype(),
                GetTag());
#endif

        break;
    case PureRT::ChannelEndpointType::DIRECT_BATCHER:
        assert(direct_batcher_temp_user_recv_buf == nullptr);
        direct_batcher_temp_user_recv_buf = user_buf;
        // otherwise, oddly, do nothing as the MPI_Irecv is implicitely done
        break;
    case PureRT::ChannelEndpointType::PROCESS:
        // fprintf(stderr, KCYN "[r%d] Calling DEQUEUE on PC %p\n" KRESET,
        // pure_thread->Get_rank(), process_channel);
        process_channel->Dequeue(user_buf);
        break;
    case PureRT::ChannelEndpointType::SEND_TO_SELF:
        send_to_self_channel->Dequeue(user_buf, NumPayloadBytes());
        break;
    case PureRT::ChannelEndpointType::RDMA_MPI:
        rdma_mpi_channel->Dequeue(user_buf);
        break;
    default:
        sentinel("Invalid channel type %d", GetChannelType());
    }

#if DEBUG_CHECK
    receiver_state.JustDidDequeue();
#endif

#if TRACE_COMM
    pure_thread_trace_comm_outstream
            << ", " /* file:line */
            << ", " /* function */
            << "recv, " << GetSenderPureRank() << ", " << GetDestPureRank()
            << ", " << count << ", " << static_cast<unsigned int>(datatype)
            << ", " << tag << ", " << GetDatatypeBytes() << ", "
            << GetDatatypeBytes() * count << ", "
            << "Dequeue"
            << ", " << GetChannelTypeName() << ", " << channel_endpoint_tag
            << ", " << (mpi_transport ? "MPI_YES" : "MPI_NO") << std::endl;
#endif
}

void RecvChannel::DequeueBlocking(const buffer_t user_buf) {

    bool mpi_transport = false;

    switch (GetChannelType()) {
    case PureRT::ChannelEndpointType::PROCESS:
        process_channel->DequeueBlocking(user_buf);
        break;
    case PureRT::ChannelEndpointType::DIRECT_MPI:
        direct_mpi_channel->DequeueBlocking(user_buf);
        mpi_transport = true;
        break;
    default:
        sentinel("Invalid channel type %d for DequeueBlocking call",
                 GetChannelType());
    }

#if DEBUG_CHECK
    receiver_state.JustDidBlockingDequeue();
#endif

#if TRACE_COMM
    pure_thread_trace_comm_outstream
            << ", " /* file:line */
            << ", " /* function */
            << "recv, " << GetSenderPureRank() << ", " << GetDestPureRank()
            << ", " << count << ", " << static_cast<unsigned int>(datatype)
            << ", " << tag << ", " << GetDatatypeBytes() << ", "
            << GetDatatypeBytes() * count << ", "
            << "Dequeue"
            << ", " << GetChannelTypeName() << ", " << channel_endpoint_tag
            << ", " << (mpi_transport ? "MPI_YES" : "MPI_NO") << std::endl;
#endif
}

bool RecvChannel::BufferValid() {

#if DEBUG_CHECK
    receiver_state.AssertUsableBuffer();
#endif

    PureRT::ChannelEndpointType ct;
    switch (ct = GetChannelType()) {
    // We put BUNDLE first as that's probably the common case
    case PureRT::ChannelEndpointType::PROCESS:
        return true;
    case PureRT::ChannelEndpointType::DIRECT_MPI:
        return true;
    case PureRT::ChannelEndpointType::RDMA_MPI:
        return true;
    default:
        sentinel("Invalid channel type %d", ct);
    }
}

const PureContRetArr& RecvChannel::Wait() {

#if DEBUG_CHECK
    // this must go up top as the switch statement returns directly.
    receiver_state.JustDidWait();
#endif

    bool dequeue_succeeded;
    switch (GetChannelType()) {
    case PureRT::ChannelEndpointType::DIRECT_MPI:
        return direct_mpi_channel->DequeueWait();
    case PureRT::ChannelEndpointType::FLAT_BATCHER:
#if FLAT_BATCHER_RECV_VERSION == 1
        WaitForFlatBatcherCompletion();
#endif
#if FLAT_BATCHER_RECV_VERSION == 2
        assert(flat_batcher_temp_user_recv_buf != nullptr);
        recv_flat_batcher->WaitRecvRequest(
                pure_thread->GetThreadNumInProcess(), this,
                flat_batcher_temp_user_recv_buf, NumPayloadBytes(),
                GetSenderPureRank(), GetDestPureRank(), GetDatatype(),
                GetTag());
        flat_batcher_temp_user_recv_buf = nullptr; // reset
#endif
        return static_empty_pure_cont_ret_arr;
    case PureRT::ChannelEndpointType::DIRECT_BATCHER:
        // direct_batcher_temp_user_recv_buf should be set in Dequeue
        assert(direct_batcher_temp_user_recv_buf != nullptr);
        DirectBatcherRecv();
        // reset for next time to make sure we're not doing multiple
        // dequeues at once
        direct_batcher_temp_user_recv_buf = nullptr;
        // no pipelines supported, currently, with batcher channels. should
        // assert this somewhere.
        return static_empty_pure_cont_ret_arr;
    case PureRT::ChannelEndpointType::RDMA_MPI:
        return rdma_mpi_channel->DequeueWait();
    case PureRT::ChannelEndpointType::SEND_TO_SELF:
        send_to_self_channel->DequeueWait();
        return static_empty_pure_cont_ret_arr;
    case PureRT::ChannelEndpointType::PROCESS:
        return process_channel->DequeueWait();
    default:
        sentinel("Invalid channel type %d", GetChannelType());
    }
}

#if FLAT_BATCHER_RECV_VERSION == 1
void RecvChannel::WaitForFlatBatcherCompletion() const {
    while (flat_batcher_msg_complete.load(std::memory_order_acquire) == false) {
        // work steal?
        x86_pause_instruction();
    }
}
#endif

bool RecvChannel::Test() {

#if DEBUG_CHECK
    // this must go up top as the switch statement returns directly.
    receiver_state.JustDidWait();
#endif

    bool       dequeue_succeeded = false;
    const auto do_blocking_wait  = false;

    switch (GetChannelType()) {
    case PureRT::ChannelEndpointType::DIRECT_MPI:
        direct_mpi_channel->DequeueWait(do_blocking_wait, &dequeue_succeeded);
        break;
    case PureRT::ChannelEndpointType::PROCESS:
        process_channel->DequeueWait(do_blocking_wait, &dequeue_succeeded);
        break;
    default:
        sentinel("Invalid channel type %d", GetChannelType());
    }

    // make sure set for process channel
    return dequeue_succeeded;
}

#if FLAT_BATCHER_RECV_VERSION == 1
void RecvChannel::WaitForFlatBatcherCompletion() const {
    while (flat_batcher_msg_complete.load(std::memory_order_acquire) == false) {
        // work steal?
        x86_pause_instruction();
    }
}
#endif

void RecvChannel::DirectBatcherRecv() {

    bool       dequeue_succeeded, became_irecv_leader;
    const auto sender_mpi_rank =
            pure_thread->MPIRankFromPureRank(GetSenderPureRank());
    auto batcher_manager =
            pure_thread->GetDequeueBatcherManager(sender_mpi_rank);
    const auto num_payload_bytes = NumPayloadBytes();

    DirectChannelBatcher* curr_batcher = batcher_manager->GetCurrBatcher();
    do {
        // wait until it's ok to go in
        // I think this may be unnecessary as the entry gets done before the
        // batcher gets incremented.
        curr_batcher->WaitUntilDequeueEntryAllowed();

        dequeue_succeeded =
                curr_batcher->DequeueWaitAndTryToConsumeSingleMessage(
                        direct_batcher_temp_user_recv_buf, num_payload_bytes,
                        GetSenderPureRank(), GetDestPureRank(), GetDatatype(),
                        GetTag(), pure_thread->GetThreadNumInProcess(),
                        became_irecv_leader);

        // if I became the irecv leader, then I have to kick off the next
        // irecv.
        if (became_irecv_leader) {
            // 1. get the next batcher
            DirectChannelBatcher* next_batcher =
                    batcher_manager->GetNextBatcher();

            // 2. Turn off access to this batcher (although some will still
            // be in it for some time)
            next_batcher->DisallowDequeueEntry();

            // 3. Wait until no threads are in this one as it may take some
            // time for them to drain out
            next_batcher->WaitUntilNoActiveDequeueThreads();

            //////////////////////////////////////////
            // 4. kick off the irecv
            next_batcher->DoDequeueMpiIrecv();

            if (BATCHER_DEBUG_PRINT) {
                fprintf(stderr,
                        "r%d  am the new irecv leader. setting batcher "
                        "from %p "
                        "to "
                        "%p "
                        "and resetting batcher %p\n",
                        pure_thread->Get_rank(),
                        batcher_manager->GetCurrBatcher(), next_batcher,
                        next_batcher);
            }

            // 5. reset the batcher
            next_batcher->ResetDequeue();
            //////////////////////////////////////////

            // 6. Re-enable access to this batcher
            next_batcher->AllowDequeueEntry();

            // 7. update the current dequeue batcher
            batcher_manager->IncrementBatcher();
        } // ends became recv leader

        if (dequeue_succeeded == false) {
            // if we didn't find our message, don't try again until the
            // batcher gets updated as we scanned the entire message and it
            // wasn't there.
            DirectChannelBatcher* new_batcher;
            while ((new_batcher = batcher_manager->GetCurrBatcher()) ==
                   curr_batcher) {
                x86_pause_instruction();
            }
            curr_batcher = new_batcher;
        } // ends dequeue failed -- need to try again with a new batcher

    } while (dequeue_succeeded == false);
}

#if !DO_LIST_WAITALL
// static
void RecvChannel::WaitAll(int count, RecvChannel* const* const rcs,
                          PureThread* const pure_thread) {

    if (count == 0) {
        return;
    }

    bool completed[count];
    std::fill(completed, completed + count, false);

    int remaining = count;
    while (remaining > 0) {
        for (auto i = 0; i < count; ++i) {
            if (completed[i]) {
                continue;
            }

            assert(rcs[i] != nullptr);
            assert(rcs[i]->GetChannelType() ==
                           PureRT::ChannelEndpointType::PROCESS ||
                   rcs[i]->GetChannelType() ==
                           PureRT::ChannelEndpointType::DIRECT_MPI);

            const bool do_blocking_wait = false;
            bool       dequeue_succeeded;

            if (rcs[i]->GetChannelType() ==
                PureRT::ChannelEndpointType::PROCESS) {
                const PureContRetArr& ret =
                        rcs[i]->process_channel->DequeueWait(
                                do_blocking_wait, &dequeue_succeeded);
                check(ret == static_empty_pure_cont_ret_arr,
                      "WaitAll doesn't currently support continuation "
                      "return "
                      "values.");
            } else {
                // direct channel
                const PureContRetArr& ret =
                        rcs[i]->direct_mpi_channel->DequeueWait(
                                do_blocking_wait, &dequeue_succeeded);
                check(ret == static_empty_pure_cont_ret_arr,
                      "WaitAll doesn't currently support continuation "
                      "return "
                      "values.");
            }

            if (dequeue_succeeded) {
                completed[i] = true;
                --remaining;
            }
        }
    }
}

#endif

// static
#if DO_LIST_WAITALL
void RecvChannel::WaitAll(int count, RecvChannel* const* const rcs,
                          PureThread* const pure_thread) {
    if (count == 0) {
        return;
    }

    std::list<RecvChannel*> remain_rcs(rcs, rcs + count);
    const int               should_process = count;
    int                     did_process    = 0;
    bool                    successful_dequeue_this_pass;

    while (remain_rcs.size() > 0) {
        successful_dequeue_this_pass = false; // reset
        for (auto rc_it = remain_rcs.begin(); rc_it != remain_rcs.end();) {
            RecvChannel* rc = *rc_it;
            assert(rc != nullptr);
            assert(rc->GetChannelType() ==
                           PureRT::ChannelEndpointType::PROCESS ||
                   rc->GetChannelType() ==
                           PureRT::ChannelEndpointType::DIRECT_MPI);

            const bool do_blocking_wait = false;
            bool       dequeue_succeeded;

            if (rc->GetChannelType() == PureRT::ChannelEndpointType::PROCESS) {
                const PureContRetArr& ret = rc->process_channel->DequeueWait(
                        do_blocking_wait, &dequeue_succeeded);
                check(ret == static_empty_pure_cont_ret_arr,
                      "WaitAll doesn't currently support continuation "
                      "return "
                      "values.");
            } else {
                // direct channel
                const PureContRetArr& ret = rc->direct_mpi_channel->DequeueWait(
                        do_blocking_wait, &dequeue_succeeded);
                check(ret == static_empty_pure_cont_ret_arr,
                      "WaitAll doesn't currently support continuation "
                      "return "
                      "values.");
            }

            if (dequeue_succeeded) {
                // remove this entry from the list
                auto to_del = rc_it;
                ++rc_it; // advance before deleting it
                remain_rcs.erase(to_del);
                ++did_process;
                successful_dequeue_this_pass = true;
            } else {
                // just advance it -- come back later
                ++rc_it;
            }
        } // for all in list

#if DO_WAITALL_WS
        if (remain_rcs.size() > 0 && !successful_dequeue_this_pass) {
            MAYBE_WORK_STEAL(pure_thread);
        }
#endif
    }
    assert(did_process == should_process);
}
#endif

// this is a hack, but you must pass in a list of rcs to keep track of completed
// ones across subsequent calls

// init:
//     std::list<RecvChannel*> remain_rcs(rcs, rcs + count);

int RecvChannel::WaitAny(int count, RecvChannel* const* const rcs,
                         PureThread* const        pure_thread,
                         std::list<RecvChannel*>& remain_rcs) {

    if (count <= 0) {
        print_pure_backtrace();
        sentinel("count for waitany must be >0 but is %d\n", count);
    }

    bool successful_dequeue_this_pass;

    while (remain_rcs.size() > 0) {
        successful_dequeue_this_pass = false; // reset
        for (auto rc_it = remain_rcs.begin(); rc_it != remain_rcs.end();) {
            RecvChannel* rc = *rc_it;
            assert(rc != nullptr);
            assert(rc->GetChannelType() ==
                           PureRT::ChannelEndpointType::PROCESS ||
                   rc->GetChannelType() ==
                           PureRT::ChannelEndpointType::DIRECT_MPI);

            const bool do_blocking_wait = false;
            bool       dequeue_succeeded;

            if (rc->GetChannelType() == PureRT::ChannelEndpointType::PROCESS) {
                const PureContRetArr& ret = rc->process_channel->DequeueWait(
                        do_blocking_wait, &dequeue_succeeded);
                check(ret == static_empty_pure_cont_ret_arr,
                      "WaitAll doesn't currently support continuation "
                      "return "
                      "values.");
            } else {
                // direct channel
                const PureContRetArr& ret = rc->direct_mpi_channel->DequeueWait(
                        do_blocking_wait, &dequeue_succeeded);
                check(ret == static_empty_pure_cont_ret_arr,
                      "WaitAll doesn't currently support continuation "
                      "return "
                      "values.");
            }

            if (dequeue_succeeded) {
                // remove this entry from the list
                const auto idx    = rc->GetIndex();
                auto       to_del = rc_it;
                ++rc_it; // advance before deleting it
                remain_rcs.erase(to_del);
                successful_dequeue_this_pass = true;
                return idx;
            } else {
                // just advance it -- come back later
                ++rc_it;
            }
        } // for all in list

#if DO_WAITALL_WS
        if (remain_rcs.size() > 0 && !successful_dequeue_this_pass) {
            MAYBE_WORK_STEAL(pure_thread);
        }
#endif
    }
    sentinel("Should not return here");
    return -1;
}

// TODO: if this works, maybe turn this into a simple class
// bool completed[count] all set to false;
//     std::fill(completed, completed + count, false);
//     int remaining = count;
#if 0
int RecvChannel::WaitAny(int count, RecvChannel* const* const rcs,
                         PureThread* const pure_thread, bool* completed,
                         int& remaining) {
    assert(count > 0);

    while (remaining > 0) {
        for (auto i = 0; i < count; ++i) {
            if (completed[i]) {
                continue;
            }

            assert(rcs[i] != nullptr);
            assert(rcs[i]->GetChannelType() ==
                           PureRT::ChannelEndpointType::PROCESS ||
                   rcs[i]->GetChannelType() ==
                           PureRT::ChannelEndpointType::DIRECT_MPI);

            const bool do_blocking_wait = false;
            bool       dequeue_succeeded;

            if (rcs[i]->GetChannelType() ==
                PureRT::ChannelEndpointType::PROCESS) {
                const PureContRetArr& ret =
                        rcs[i]->process_channel->DequeueWait(
                                do_blocking_wait, &dequeue_succeeded);
                check(ret == static_empty_pure_cont_ret_arr,
                      "WaitAll doesn't currently support continuation "
                      "return "
                      "values.");
            } else {
                // direct channel
                const PureContRetArr& ret =
                        rcs[i]->direct_mpi_channel->DequeueWait(
                                do_blocking_wait, &dequeue_succeeded);
                check(ret == static_empty_pure_cont_ret_arr,
                      "WaitAll doesn't currently support continuation "
                      "return "
                      "values.");
            }

            if (dequeue_succeeded) {
                completed[i] = true;
                --remaining;
                return i;
            }
        }
    }
}
#endif

bool RecvChannel::HasBufferInitializerFunc() const {
    return buf_init_func != nullptr;
}

void RecvChannel::InitBuffer(void* buf) { buf_init_func(buf); }

void RecvChannel::Validate() const {
#if DEBUG_CHECK
    if (GetChannelType() == PureRT::ChannelEndpointType::DIRECT_MPI) {
        direct_mpi_channel->Validate();
    }

#endif
}
