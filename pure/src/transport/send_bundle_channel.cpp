// Author: James Psota
// File:   send_bundle_channel.cpp 

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

#include "pure/transport/send_bundle_channel.h"
#include "pure/transport/bundled_message.h"
#include "pure/transport/sender_channel_state.h"
#include "pure/transport/send_channel.h"

/*
PLAN

look for all uses of payloads_available_for_use_map in recv_bundle_channel and
implement analogs in send_bundle_channel.

Rationale: Fundamentally, we need to make the user program wait until it's ok to
write the user's runtime buffer again. This will likely be used right before the
user writes to the buffer again. (Note: is there some way to automate this so there's
not another user call necessary?) There are a couple approaches to leverage the code:

* don't -- just copy for now
* introduce another layer of inheritance and refactor
* just put it in BundleChannel and don't worry about it

*/

SendBundleChannel::SendBundleChannel(int dest_mpi_rank_arg, int total_num_messages_in_bundle_arg,
                                     int mpi_message_tag_arg)
    : BundleChannel(dest_mpi_rank_arg, PureRT::EndpointType::SENDER, total_num_messages_in_bundle_arg,
                    mpi_message_tag_arg) {}

SendBundleChannel::~SendBundleChannel() {
    for (auto& rank_cf_pair : buffer_available_for_writing_map) {
        delete (rank_cf_pair.second);
    }
}

void SendBundleChannel::BindChannelEndpoint(BundleChannelEndpoint* bce) {

    bool child_class_does_more_initialization = true;
    bool all_endpoints_bound_by_me = BundleChannel::BaseBindChannelEndpoint(bce, child_class_does_more_initialization);

    if (all_endpoints_bound_by_me) {

        // initialize data structure for Enqueue()
        for (auto i = 0; i < num_bound_channel_endpoints; ++i) {
            int rank = bound_channel_endpoints[i]->GetSenderPureRank();
            SendChannel* this_sc = dynamic_cast<SendChannel*>(bound_channel_endpoints[i]);
            this_sc->SetInitializedState();

            // initialize an entry of a map of ConditionFrames for telling waiting threads that
            // it's ok to start writing into the buffer again for the next enqueue.
            buffer_available_for_writing_map[rank] = new ConditionFrame{true /* ready before Enqueue ever called */};
        }

        // note: a side effect of the BundledMessage constructor is to set the runtime send
        // buffer on each bound channel endpoint object.
        bundled_message = new BundledMessage(endpoint_type, total_num_messages_in_bundle, bound_channel_endpoints);
        SignalAllThreadsInitialized();
    }
}

bool SendBundleChannel::Enqueue(int sender_pure_rank) {

    AssertInvarients();
    WaitAllThreadsReady(sender_pure_rank);

    bool msg_sent = false;
    buffer_available_for_writing_map[sender_pure_rank]->MarkNotReady();

    ts_log_info("Enqueue[rank %d]: marked NOT ready: %p", sender_pure_rank,
                buffer_available_for_writing_map[sender_pure_rank]);

    // START////////////////////////// CRITICAL SECTION
    {
        // STEP 2: update the number of messages in this bundle
        std::unique_lock<std::mutex> num_messages_in_bundle_lock(num_messages_in_bundle_mutex);

        // TODO(jim): optimization: this entire section could be done with an atomic
        // curr_num_message_in_bundle
        // and without a lock, possibly. Try it.
        // note: ripped this out...
        // bundled_message->CopyMessageData(sender_pure_rank);

        ++curr_num_messages_in_bundle;
        assert(curr_num_messages_in_bundle <= total_num_messages_in_bundle);

        if (curr_num_messages_in_bundle == total_num_messages_in_bundle) {
            // at this point, all of the message data has been added, so send the
            // message.

            // ts_log_warn("Before MPI_Send. Here are the buffer contents (this is rank %d here):", sender_pure_rank);
            // fprintf(stderr, "%s\n", debug_array_to_string<int>(static_cast<int*>(bundled_message->BufPointerStart()),
            // bundled_message->TotalBufSizeBytes()/sizeof(int), std::string("send_bundled_message")).c_str());

            ts_log_info(KYEL_BG "[CET %d] About to MPI_Send %d bytes to process %d" KRESET,
                        bound_channel_endpoints[0]->GetChannelEndpointTag(), bundled_message->TotalBufSizeBytes(),
                        endpoint_mpi_rank);
            MPI_Send(bundled_message->BufPointerStart(), bundled_message->TotalBufSizeBytes(),
                     BUNDLE_CHANNEL_MPI_DATATYPE, endpoint_mpi_rank, mpi_message_tag, MPI_COMM_WORLD);

            for (auto& rank_cf_pair : buffer_available_for_writing_map) {
                // notify this thread
                rank_cf_pair.second->MarkReadyAndNotifyOne();
                ts_log_info("Enqueue[rank %d]: marked READY: %p", sender_pure_rank,
                            rank_cf_pair.second);
            }

            msg_sent = true;
            ResetState();
            SignalAllThreadsReady(); // note: num_messages_in_bundle_mutex must be held to call this
                                     // function (it is, above)
        }                            // ends if(curr_num_messages_in_bundle == total_num_messages_in_bundle)
        assert(curr_num_messages_in_bundle <= total_num_messages_in_bundle);
    }
    // END//////////////////////////// CRITICAL SECTION

    return msg_sent;

} // function Enqueue

bool SendBundleChannel::BufferWritable(int pure_sender_rank) {
    bool this_rank_ready_for_writing = buffer_available_for_writing_map[pure_sender_rank]->Ready();
    ts_log_info("BufferWritable[rank %d]: marking ready: %p", pure_sender_rank,
                buffer_available_for_writing_map[pure_sender_rank]);
    return this_rank_ready_for_writing;
}

void SendBundleChannel::WaitUntilBufWritable(int pure_sender_rank) {
    AssertInvarients();
    buffer_available_for_writing_map[pure_sender_rank]->WaitForReadyUnlessAlreadyReady();
    ts_log_info("WaitUntilBufWritable[rank %d]: writable %d: %p", pure_sender_rank,
                buffer_available_for_writing_map[pure_sender_rank],
                buffer_available_for_writing_map[pure_sender_rank]->Ready());
    assert(BufferWritable(pure_sender_rank));
}
