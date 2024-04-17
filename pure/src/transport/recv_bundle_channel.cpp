// Author: James Psota
// File:   recv_bundle_channel.cpp 

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

#include "pure/transport/recv_bundle_channel.h"
#include "pure/support/zed_debug.h"
#include "pure/transport/bundle_channel_endpoint.h"
#include "pure/transport/bundled_message.h"
#include "pure/transport/recv_channel.h"
#include "pure/transport/mpi_message_metadata.h"

RecvBundleChannel::RecvBundleChannel(int dest_mpi_rank_arg, int total_num_messages_in_bundle_arg,
                                     int mpi_message_tag_arg)
    : BundleChannel(dest_mpi_rank_arg, PureRT::EndpointType::RECEIVER, total_num_messages_in_bundle_arg,
                    mpi_message_tag_arg) {}

RecvBundleChannel::~RecvBundleChannel() {
    for (auto& rank_cf_pair : payloads_available_for_use_map) {
        delete (rank_cf_pair.second);
    }
}

void RecvBundleChannel::BindChannelEndpoint(BundleChannelEndpoint* bce) {

    bool child_class_does_more_initialization = true;
    bool all_endpoints_bound_by_me = BundleChannel::BaseBindChannelEndpoint(bce, child_class_does_more_initialization);

    if (all_endpoints_bound_by_me) {

        // initialize data structure for Dequeue()
        for (auto i = 0; i < num_bound_channel_endpoints; ++i) {
            int          rank = bound_channel_endpoints[i]->GetDestPureRank(); // dest, as this is a recv bundle channel
            RecvChannel* this_rc = dynamic_cast<RecvChannel*>(bound_channel_endpoints[i]);
            this_rc->SetInitializedState();

            // initialize an entry of a map of ConditionFrames for telling waiting threads that the payloads
            // owned by their thread are available for use.
            auto iter = payloads_available_for_use_map.find(rank);
            if (iter == payloads_available_for_use_map.end()) {
                auto cf               = new ConditionFrame{};
                auto emplace_ret_pair = payloads_available_for_use_map.emplace(rank, cf);
                assert(emplace_ret_pair.second); // assert insertion success
            }
        } // ends for each bound channel endpoint

        // note: a side effect of the BundledMessage constructor is to set the runtime recv
        // buffer on each bound channel endpoint object.
        bundled_message = new BundledMessage(endpoint_type, total_num_messages_in_bundle, bound_channel_endpoints);
        SignalAllThreadsInitialized();
    }
}

bool RecvBundleChannel::Dequeue(int sender_pure_rank, int dest_pure_rank) {

    AssertInvarients();
    // TODO(jim): this wasn't heavily vetted. If this blocks on DT here, then look at the old code and
    // consider using that structure (both sender and dest ranks in a unified key)
    WaitAllThreadsReady(sender_pure_rank); // TODO(jim): will this work as it did in the old dequeue? work
                                           // through this with an example.

    bool message_received_from_network = false;
    payloads_available_for_use_map[dest_pure_rank]->MarkNotReady();

    {
        std::unique_lock<std::mutex> num_messages_in_bundle_lock(num_messages_in_bundle_mutex);
        ++curr_num_messages_in_bundle;
        assert(curr_num_messages_in_bundle <= total_num_messages_in_bundle);

        if (curr_num_messages_in_bundle == total_num_messages_in_bundle) {
            // at this point, all of the receivers are ready to receive the message (actually use
            // the buffer), so
            // receive it. Effectively, the Dequeue is a signal that means the user is done with the
            // buffer from the
            // previous iteration, so it's time to do the MPI_Recv.
            int        mpi_recv_ret;
            MPI_Status status{};

            ts_log_warn(KYEL_BG "[CET %d] About to MPI_Recv %d bytes from process rank %d" KRESET,
                        bound_channel_endpoints[0]->GetChannelEndpointTag(), bundled_message->TotalBufSizeBytes(),
                        endpoint_mpi_rank);

            mpi_recv_ret =
                    MPI_Recv(bundled_message->BufPointerStart(), bundled_message->TotalBufSizeBytes(),
                             BUNDLE_CHANNEL_MPI_DATATYPE, endpoint_mpi_rank, mpi_message_tag, MPI_COMM_WORLD, &status);
            assert(mpi_recv_ret == MPI_SUCCESS);

// ts_log_warn("Done MPI_Recv. Here are the buffer contents:");
// fprintf(stderr, "%s\n", debug_array_to_string<int>(static_cast<int*>(bundled_message->BufPointerStart()),
// bundled_message->TotalBufSizeBytes()/sizeof(int), std::string("recv_bundled_message")).c_str());

// signal dest_pure_rank thread to wake up if this is it's last message. otherwise,
// just update the bookkeeping.

#if DEBUG_CHECK
            // check that the header words that came through are correct.
            char* bundle_buf_curr_ptr       = static_cast<char*>(bundled_message->BufPointerStart());
            char* debug_bundle_buf_curr_ptr = static_cast<char*>(bundled_message->GetDebugBundleBufStart());

            for (int i = 0; i < total_num_messages_in_bundle; ++i) {
                BundleChannelEndpoint* bce = bound_channel_endpoints[i];

                // http://www.cplusplus.com/reference/cstring/memcmp/
                // return value of zero means they are the same
                assert(memcmp(debug_bundle_buf_curr_ptr, bundle_buf_curr_ptr, MPIMessageMetadata::MetadataBufLen) == 0);

                // skip both the header and payload for the i-th entry to compare the next header
                // value.
                int skip_bytes = MPIMessageMetadata::MetadataBufLen + bce->NumPayloadBytes();
                bundle_buf_curr_ptr += skip_bytes;
                debug_bundle_buf_curr_ptr += skip_bytes;
            }
#endif

            // TODO(jim): there's probably a better way, but deal with this after the refactor to use
            // per-thread variables for bookkeeping
            for (auto& rank_cf_pair : payloads_available_for_use_map) {
                // notify this thread
                rank_cf_pair.second->MarkReadyAndNotifyOne();
            }

            message_received_from_network = true;
            ResetState();

            // important: payload_available_cf must happen before SignalAllThreadsReady
            // SignalAllThreadsReady signals threads already on the next iteration of Dequeue to go.
            SignalAllThreadsReady(); // note: num_messages_in_bundle_mutex must be held to call this
                                     // function (it is, above).

        } // ends if(curr_num_messages_in_bundle == total_num_messages_in_bundle)
        assert(curr_num_messages_in_bundle <= total_num_messages_in_bundle);
    }

    return message_received_from_network;

} // function Dequeue

bool RecvBundleChannel::BufferValid(int pure_dest_rank) {
    return payloads_available_for_use_map[pure_dest_rank]->Ready();
}

void RecvBundleChannel::Wait(int pure_dest_rank) {
    AssertInvarients();
    payloads_available_for_use_map[pure_dest_rank]->WaitForReadyUnlessAlreadyReady();
}

void RecvBundleChannel::SetAllRecvChannelsInSameThreadToWait(int pure_rank) {
    for (auto i = 0; i < num_bound_channel_endpoints; ++i) {
        RecvChannel& this_rc = *dynamic_cast<RecvChannel*>(bound_channel_endpoints[i]);
        if (this_rc.GetDestPureRank() == pure_rank) {
            // this means that this receive channel has the same rank as the calling thread,
            // so we're going to update its receive_channel_state to the "wait" state.
            // note that no locking is needed as we're only updating RecvChannels owned
            // by this thread.
            assert(this_rc.GetDestPureRank() == pure_rank);
            ts_log_info("### RBC[%d/%d]: me: %d setting rank %d, rc (%p) curr state: %d", i,
                        num_bound_channel_endpoints-1, pure_rank, this_rc.GetDestPureRank(), &this_rc,
                        this_rc.GetChannelState());

            this_rc.SetWaitedState();
        }
    }
}