// Author: James Psota
// File:   bundle_channel.cpp 

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

#include "pure/transport/bundle_channel.h"
#include "pure/support/helpers.h"
#include "pure/transport/bundle_channel_endpoint.h"
#include "pure/transport/bundled_message.h"
#include "pure/transport/mpi_message_metadata.h"

std::atomic<int> BundleChannel::num_local_bundle_channels{0};

BundleChannel::BundleChannel(int                  endpoint_mpi_rank_arg,
                             PureRT::EndpointType endpoint_type_arg,
                             int total_num_messages_in_bundle_arg,
                             int mpi_message_tag_arg)

    : // initializer list
      curr_num_messages_in_bundle(0),
      total_num_messages_in_bundle(total_num_messages_in_bundle_arg),
      endpoint_type(endpoint_type_arg),
      endpoint_mpi_rank(endpoint_mpi_rank_arg),
      mpi_message_tag(mpi_message_tag_arg),
      num_bundled_messages_sent_or_received(0) {

    check(endpoint_type == PureRT::EndpointType::SENDER ||
                  endpoint_type == PureRT::EndpointType::RECEIVER ||
                  endpoint_type == PureRT::EndpointType::REDUCE ||
                  endpoint_type == PureRT::EndpointType::BCAST,
          "Invalid enpoint type.");

    num_bound_channel_endpoints = 0;

    if (total_num_messages_in_bundle != BUNDLE_CHANNEL_UNUSED_NUM_MESSAGES) {
        // this is the case where we don't bind channel endpoints to the bundle
        // channel. In some cases (e.g., bcast) we don't need this, and to keep
        // things simple, we don't do the binding.
        bound_channel_endpoints =
                new BundleChannelEndpoint*[total_num_messages_in_bundle];
    }
    initializer_thread_num = -1000;
    initialized_for_use    = false;
    bundled_message        = nullptr;

    int barrier_init_res =
            pthread_barrier_init(&wait_for_all_binders_complete_barrier,
                                 nullptr, total_num_messages_in_bundle);
    assert(barrier_init_res == 0);

    ++BundleChannel::num_local_bundle_channels;
} // constructor

BundleChannel::~BundleChannel() {

    if (bundled_message != nullptr) {
        delete bundled_message;
    }

    if (total_num_messages_in_bundle != BUNDLE_CHANNEL_UNUSED_NUM_MESSAGES) {
        delete[] bound_channel_endpoints;
    }
    pthread_barrier_destroy(&wait_for_all_binders_complete_barrier);

    /*
    TODO: rip out after refactor
        for (auto& it : all_payloads_available_for_use_map) {
            delete (it.second);
        }
    */

} // destructor

bool BundleChannel::BaseBindChannelEndpoint(BundleChannelEndpoint* bce,
                                            bool more_child_initialization) {

// if more_child_initialization is true, that means the child class needs to do
// more initialization and is responsible for calling
// initializer_done_initializing_cf.MarkReadyAndNotifyAll();

// TODO(jim): refactor this after incorporating ReceiveBundleChannel to do all
// of the data structure initialization that is unique to the subclass (e.g.,
// SendBundleChannel or ReduceBundleChannel).

/*
 * Algorithm:
 *   The goal is to give this bundle channel a list of its associated
 * BundleChannelEndpoints for future reference (one of which is to be able to
 * allocate the right buffer size for its bundled message).
 */
#if PURE_DEBUG
    if (num_bound_channel_endpoints >= total_num_messages_in_bundle) {
        ts_debug("num_bound_channel_endpoints=%d", num_bound_channel_endpoints);
        ts_debug("total_num_messages_in_bundle=%d",
                 total_num_messages_in_bundle);
    }
#endif

    check(this->endpoint_type == PureRT::EndpointType::SENDER ||
                  this->endpoint_type == PureRT::EndpointType::RECEIVER ||
                  this->endpoint_type == PureRT::EndpointType::REDUCE,
          "Invalid endpoint type %d", this->endpoint_type);

    int    my_rank;
    string channel_direction_adjective;
    if (endpoint_type == PureRT::EndpointType::SENDER) {
        my_rank = bce->GetSenderPureRank(); // for debugging only
        channel_direction_adjective = "send";
    } else if (endpoint_type == PureRT::EndpointType::RECEIVER) {
        my_rank = bce->GetDestPureRank(); // for debugging only
        channel_direction_adjective = "receive";
    } else if (endpoint_type == PureRT::EndpointType::REDUCE) {
        my_rank = bce->GetSenderPureRank(); // for debugging only
        channel_direction_adjective = "reduce";
    } else {
        fprintf(stderr, "Bad endpoint type. Exiting.\n");
        exit(-10000);
    }

    assert(EndpointsAvailable());

    bool all_endpoints_bound_by_me = false;
    {
        ////////////////////// CRITICAL SECTION START
        ////////////////////////////////
        std::lock_guard<std::mutex> bound_channel_endpoints_lock(
                bound_channel_endpoints_mutex);

        bound_channel_endpoints[num_bound_channel_endpoints] = bce;
        ++num_bound_channel_endpoints;
        assert(num_bound_channel_endpoints <= total_num_messages_in_bundle);

        // if this is the last thread in the bundle, go ahead and set up the
        // rest of the BundleChannel structures needed for Enqueue() and
        // Dequeue()
        if (num_bound_channel_endpoints == total_num_messages_in_bundle) {
            all_endpoints_bound_by_me = true; // return value
            assert(num_bound_channel_endpoints == total_num_messages_in_bundle);

            // sort bound_channel_endpoints by sender_pure_rank
            std::sort(bound_channel_endpoints,
                      bound_channel_endpoints + num_bound_channel_endpoints,
                      [](BundleChannelEndpoint* a, BundleChannelEndpoint* b) {
                          return a->GetSenderPureRank() <
                                 b->GetSenderPureRank();
                      });

#if DEBUG_CHECK
            // verify that the sorting is correct
            int prev_sender_rank =
                    -1; // 0 is the actual minimum possible sender rank
            for (auto i = 0; i < num_bound_channel_endpoints; ++i) {
                int this_sender_rank =
                        bound_channel_endpoints[i]->GetSenderPureRank();
                // check strictly less then, as each sender is only allowed to
                // send one message per bundle.
                assert(prev_sender_rank < this_sender_rank);
                prev_sender_rank = this_sender_rank;
            }
#endif

            // Notify waiters that the associated BundleChannelEndpoints are
            // ready to use In some cases, the child class may have more
            // initialization to do, so we provide the more_child_initialization
            // option.
            if (!more_child_initialization) {
                SignalAllThreadsInitialized();
            }

        } else {
            // not initializer thread
            ts_log_info(
                    "thread %d NOT initializing bundle channel at address %p "
                    "(at least not "
                    "yet) as there are only %d bound channel endpoints of %d",
                    my_rank, this, num_bound_channel_endpoints,
                    total_num_messages_in_bundle);
        }
    }
    ////////////////////// CRITICAL SECTION END ///////////////////////////////

    bce->SetBundleChannel(this);
    bce->CheckInvariants();

    return all_endpoints_bound_by_me;
} // function BindChannelEndpointNewTODO

// essentially called by the user via a BundleChannelEndpoint in the user code
void BundleChannel::WaitForInitialization() const {

    // double-check locking. Note that Ready() doesn't lock. This should be ok
    // because this is only used once, upon initialization.
    if (!InitializationDone()) {
        // otherwise, wait for ready (more expensive)
        initializer_done_initializing_cf.WaitForReady();
    }
    // else, fall through and return

} // function WaitForInitialization

void BundleChannel::SignalAllThreadsInitialized(int num_threads) {
    initializer_done_initializing_cf.MarkReadyAndNotifyAll(num_threads);
}

void BundleChannel::AssertInvarients() const {
#if DEBUG_CHECK
    check(initialized_for_use || initializer_done_initializing_cf.Ready(),
          "BundleChannel must be initialized.");

    if (endpoint_type == PureRT::EndpointType::RECEIVER ||
        endpoint_type == PureRT::EndpointType::SENDER) {
        assert(bundled_message != nullptr);
        assert(bundled_message);

        // assert that the ordering of the bound channel endpoints is correct
        // (in strictly increasing sender_rank_order) note that a bundle can
        // only contain at most one message from a given sender thread.
        int prev_sender_rank = -1;
        for (auto i = 0; i < total_num_messages_in_bundle; ++i) {
            int this_sender_rank =
                    bound_channel_endpoints[i]->GetSenderPureRank();
            assert(this_sender_rank > prev_sender_rank);
            prev_sender_rank = this_sender_rank;
        }
    }

#endif
} // function AssertInvarients

// note: this can only be called if num_messages_in_bundle_mutex is held
void BundleChannel::ResetState() {
    curr_num_messages_in_bundle = 0;
    ++num_bundled_messages_sent_or_received; // TODO(jim): alpha rename to
                                             // something more general that
                                             // works with Reduce
}

void BundleChannel::SignalAllThreadsReady() {
    {
        std::unique_lock<std::mutex> op_ready_for_me_lock(
                op_ready_for_me_mutex);
        ready_thread_ranks.clear();
    }
    op_ready_for_me_cv.notify_all();
} // function SignallAllThreadsReady

// wait until previous iteration of relevant function (Enqueue, Dequeue, Reduce,
// etc.) from me, if any, is complete.
void BundleChannel::WaitAllThreadsReady(int caller_pure_thread_rank) {

    // START////////////////////////// CRITICAL SECTION
    // ///////////////////////////////////////////
    {
        std::unique_lock<std::mutex> op_ready_for_me_lock(
                op_ready_for_me_mutex);

        // TODO(jim) optimization: remove set and use a simple array of booleans
        // (or bitvector?)
        auto insert_ret   = ready_thread_ranks.insert(caller_pure_thread_rank);
        bool added_thread = insert_ret.second;

        if (!added_thread) {
            // wait for previous call to finish, as the above would have failed
            // if another operation (Enqueue, Dequeue, etc.) on this bundle
            // channel is currently happening and not yet finished.
            op_ready_for_me_cv.wait(
                    op_ready_for_me_lock, [this, caller_pure_thread_rank]() {
                        auto insert_ret = ready_thread_ranks.insert(
                                caller_pure_thread_rank);
                        bool added_sender = insert_ret.second;
                        return added_sender;
                    });
        }
    }
    // END//////////////////////////// CRITICAL SECTION
    // ///////////////////////////////////////////
} // function WaitAllThreadsReady

#ifdef DEPRECATED
bool BundleChannel::TaggedDequeueTODO(int count, const MPI::Datatype& datatype,
                                      int sender_pure_rank, int tag,
                                      int          dest_pure_rank,
                                      RecvChannel* recv_channel) {

    // bundled message must have been created by this point in the initialized
    // step. assert this.

    // TODO(jim): refactor this and rip out initialized for use after the
    // Initalized routines are done being refactored
    assert(initialized_for_use || initializer_done_initializing_cf.Ready());
    assert(bundled_message != nullptr);
    assert(bundled_message);

    bool message_received_from_network = false;

    // STEP 1: store the intent to receive this message and, if necessary, wait
    // until previous iteration is finished.
    /////////////////////////////////////////////////////////////////////////////////

#if PURE_DEBUG
    ts_log_info(stderr,
                "\n###[thread %d] Dequeue: curr_num_messages_in_bundle = %d, "
                "sender=%d, receiver=%d\n",
                dest_pure_rank, curr_num_messages_in_bundle, sender_pure_rank,
                dest_pure_rank);
#endif

    if (all_payloads_available_for_use_map[dest_pure_rank]
                ->payload_available_cv.Ready()) {
        // this was set from the last Dequeue of this BundleChannel. Reset it.
        all_payloads_available_for_use_map[dest_pure_rank]
                ->payload_available_cv.MarkNotReady(); // threadsafe
    }

    // wait until it's ok to go (until any in-progress Dequeue is happening)
    // note: this code duplicated below in the condition variable wait section

    // extra lock for this first attempt at inserted into
    // requested_messages_metadata is needed as C++'s standard libraries are not
    // thread-safe for writes. The write inside the condition variable shouldn't
    // need an extra lock because it has it's own other lock as a function of
    // the condition variable.

    // TODO(jim): hacking these arguments for now as they are not currently
    // relevant and/or implemented
    // TODO(jim): create this mapping of receiver id to desired
    // MPIMessageMetadata in BundleChannel::Initialize() and just use a set<int>
    // of recv_pure_rank to look it up

    int                 seq_id   = 99998;
    MPIMessageMetadata* metadata = new MPIMessageMetadata(
            nullptr, nullptr, count, datatype, dest_pure_rank, tag,
            sender_pure_rank, seq_id, recv_channel,
            PureRT::BufferAllocationScheme::
                    user_buffer /* TODO: rip out buffer allocation scheme */);

    int req_messages_src_dest_key =
            MakeReqMsgsKeys(sender_pure_rank, dest_pure_rank);

    {
        std::unique_lock<std::mutex> recv_ready_for_me_lock(
                recv_ready_for_me_mutex);

        auto emplace_ret = requested_messages_metadata.emplace(
                req_messages_src_dest_key, metadata);
        // ts_log_warn(KCYN "{%p} [rank %d] 2.4: inserted metadata %p with key
        // %d (%d, %d)" KRESET,
        //           this, dest_pure_rank, metadata, req_messages_src_dest_key,
        // sender_pure_rank, dest_pure_rank);

        bool added_receiver =
                emplace_ret
                        .second; /* see
                                    http://www.cplusplus.com/reference/unordered_map/unordered_map/emplace/
                                    */
        if (!added_receiver) {

            // Wait for previous dequeue to finish if a dequeue is in flight
            recv_ready_for_me_cv.wait(
                    recv_ready_for_me_lock, [this, req_messages_src_dest_key,
                                             metadata, dest_pure_rank]() {
                        auto emplace_ret = requested_messages_metadata.emplace(
                                req_messages_src_dest_key, metadata);
                        bool added_receiver =
                                emplace_ret
                                        .second; /* see
                                                    http://www.cplusplus.com/reference/unordered_map/unordered_map/emplace/
                                                    */
                        return added_receiver;
                    });
        }
    }

    // START////////////////////////// CRITICAL SECTION
    // ///////////////////////////////////////////
    {

        // STEP 2: update the number of messages in this bundle

        std::unique_lock<std::mutex> add_requested_message_lock(
                add_requested_message_mutex);
        ++curr_num_messages_in_bundle;
        assert(curr_num_messages_in_bundle <= total_num_messages_in_bundle);

        if (curr_num_messages_in_bundle == total_num_messages_in_bundle) {
            assert(requested_messages_metadata.size() ==
                   total_num_messages_in_bundle);

            // STEP 3: when all requestors for this bundle have submitted their
            // message request, receive message from the network

            // all messages for this bundle have been requested, so go ahead and
            // receive the bundled message from the network using MPI

#if DEBUG_CHECK
            // MPI Recv into temporary buffer, and then parse message out and
            // put it into caller's specified buffer
            ts_log_info(
                    "[rank %d] Dequeue doing MPI Recv: (%p, %d, %d, %d, %d)\n",
                    dest_pure_rank, bundled_message->BufPointerStart(),
                    bundled_message->TotalBufSizeBytes(),
                    static_cast<unsigned int>(BUNDLE_CHANNEL_MPI_DATATYPE),
                    endpoint_mpi_rank, mpi_message_tag);
        }
#endif

        int        mpi_recv_ret;
        MPI_Status status;
        mpi_recv_ret = MPI_Recv(bundled_message->BufPointerStart(),
                                bundled_message->TotalBufSizeBytes(),
                                BUNDLE_CHANNEL_MPI_DATATYPE, endpoint_mpi_rank,
                                mpi_message_tag, MPI_COMM_WORLD, &status);
        assert(mpi_recv_ret == MPI_SUCCESS);

        // now the recv has been done and the bundled message needs to be
        // distributed to the correct receive buffers in the corresponding
        // threads.
        char* curr_bundle_buf_ptr =
                static_cast<char*>(bundled_message->BufPointerStart());
        assert(requested_messages_metadata.size() ==
               total_num_messages_in_bundle);

        for (auto i = 0; i < total_num_messages_in_bundle; ++i) {
            int          message_count;
            int          message_count_bytes;
            unsigned int message_datatype_uint;
            int          message_dest;
            int          message_tag;
            int          message_sender;
            int          message_seq_id;

#if DEBUG_CHECK
            fprintf(stderr,
                    KGRN
                    "ABOUT TO PARSE NEW PACKET AT curr_bundle_buf_ptr %p:\n",
                    curr_bundle_buf_ptr);
            std::cerr << BundledMessage::ToString(curr_bundle_buf_ptr)
                      << std::endl
                      << KRESET;
#endif

            MPIMessageMetadata::Parse(
                    curr_bundle_buf_ptr, &message_count, &message_count_bytes,
                    &message_datatype_uint, &message_dest, &message_tag,
                    &message_sender, &message_seq_id);

            MPI_Datatype message_datatype =
                    static_cast<MPI_Datatype>(message_datatype_uint);

///////////////////////////////////////////////////////////////////////////////////////////////////////
#if DEBUG_CHECK
            MPIMessageMetadata debug_md(nullptr, nullptr, message_count,
                                        message_datatype, message_dest,
                                        message_tag, message_sender, 1000);

            /*
            ts_log_warn(KRED "[rank %d] DEQUEUE: Message %d/%d from network",
            dest_pure_rank, i + 1, total_num_messages_in_bundle);
            ts_log_warn(KRED "[rank %d] %s" KRESET, dest_pure_rank,
                        debug_md.ToString().c_str());
            */
            // look up the requested_metadata for the package that just came in.
            // So, look up the metadata for the sender/receiver pure thread rank
            // pair.
            int req_messages_src_dest_key =
                    MakeReqMsgsKeys(message_sender, message_dest);
            MPIMessageMetadata* requested_metadata =
                    requested_messages_metadata[req_messages_src_dest_key];

            assert(requested_metadata != nullptr);
            assert(message_count == requested_metadata->GetCount());
            assert(message_count_bytes == requested_metadata->GetCountBytes());
            assert(message_datatype == requested_metadata->GetDatatype());
            assert(message_sender == requested_metadata->GetPureSender());
            assert(message_dest == requested_metadata->GetPureDest());
            assert(message_tag == requested_metadata->GetTag());
#endif

            // advance pointer to start of payload of user message
            curr_bundle_buf_ptr += MPIMessageMetadata::MetadataBufLen;
            assert(requested_metadata->GetBufferAllocationScheme() ==
                   PureRT::BufferAllocationScheme::user_buffer);

#if DEBUG_CHECK
            // make sure the value of the channel_buf doesn't change through the
            // lifetime of this channel
            if (num_bundled_messages_sent_or_received > 0) {
                RecvChannel* rc = requested_metadata->GetRecvChannel();
                check(rc->GetRecvChannelBuf<void*>() == curr_bundle_buf_ptr,
                      "The value of the channel buf "
                      "should not change through the "
                      "life of this channel.");
                check(rc->NumPayloadBytes() == message_count_bytes,
                      "The number of bytes on the incoming message should "
                      "match the number of "
                      "bytes in the RecvChannel");
            }
#endif
            if (num_bundled_messages_sent_or_received == 0) {
                // only need to set this on the first Dequeue. Note tha that the
                // above DEBUG_CHECK verifies that this value doesn't change.
                requested_metadata->GetRecvChannel()->SetRecvChannelBuf(
                        curr_bundle_buf_ptr);
            }
            // memcpy(requested_metadata->GetOrigBuf(), curr_bundle_buf_ptr,
            // message_count_bytes);

            // signal message_dest thread to wake up if this is it's last
            // message. otherwise, just update the bookkeeping.
            all_payloads_available_for_use_map[message_dest]
                    ->num_messages_parsed_this_time += 1;
            if (all_payloads_available_for_use_map[message_dest]
                        ->num_messages_parsed_this_time ==
                all_payloads_available_for_use_map[message_dest]
                        ->total_num_messages_in_bundle_for_me) {

                all_payloads_available_for_use_map[message_dest]
                        ->payload_available_cv.MarkReadyAndNotifyOne();

                // reset counter for next time
                all_payloads_available_for_use_map[message_dest]
                        ->num_messages_parsed_this_time = 0;

            } // ends if this is the last message to be received in this bundle
              // for this thread

            curr_bundle_buf_ptr += message_count_bytes;
        } // for all message in the requested_messages_metadata map

        {
            std::unique_lock<std::mutex> recv_ready_for_me_lock(
                    recv_ready_for_me_mutex);

            // clear out the requested_messages_map for next time
            for (auto& it : requested_messages_metadata) {
                // annoyingly, requested_messages_metadata.clear() doesn't seem
                // to call the destructor for the values in this map...
                delete (it.second);
            }
            requested_messages_metadata.clear();
            curr_num_messages_in_bundle   = 0;
            message_received_from_network = true;
            ++num_bundled_messages_sent_or_received;
        }
        recv_ready_for_me_cv.notify_all();

    } // if(curr_num_messages_in_bundle == total_num_messages_in_bundle)
}
// END//////////////////////////// CRITICAL SECTION
// ///////////////////////////////////////////

ts_log_warn("[rank %d] 1000: Returning %d", dest_pure_rank,
            message_received_from_network);

return message_received_from_network;

} // ends function TaggedDequeueTODO

bool BundleChannel::Dequeue(void* const buf, int count,
                            const MPI::Datatype& datatype, int sender_pure_rank,
                            int tag, int dest_pure_rank,
                            RecvChannel* recv_channel) {

    // bundled message must have been created by this point

    // TODO(jim): refactor this and rip out initialized for use after the
    // Initalized routines are done being refactored
    assert(initialized_for_use || initializer_done_initializing_cf.Ready());
    assert(bundled_message != nullptr);
    assert(bundled_message);

    bool message_received_from_network = false;

    // STEP 1: store the intent to receive this message
    /////////////////////////////////////////////////////////////////////////////////

    // TODO(jim): hacking these arguments for now as they are not currently
    // relevant and/or implemented
    // TODO(jim): create this mapping of receiver id to desired
    // MPIMessageMetadata in BundleChannel::Initialize() and just use a set<int>
    // of recv_pure_rank to look it up
    int                 seq_id   = 99998;
    MPIMessageMetadata* metadata = new MPIMessageMetadata(
            buf, nullptr, count, datatype, dest_pure_rank, tag,
            sender_pure_rank, seq_id, recv_channel,
            PureRT::BufferAllocationScheme::
                    user_buffer /* TODO: rip out buffer allocation scheme */);

#if PURE_DEBUG
    sleep_random_seconds(150);
    fprintf(stderr,
            "\n###[thread %d] Dequeue: curr_num_messages_in_bundle = %d, "
            "sender=%d, receiver=%d\n",
            dest_pure_rank, curr_num_messages_in_bundle, sender_pure_rank,
            dest_pure_rank);
#endif

    // TODO(jim): much change this to use channel endpoint tag when using tagged
    // channel endpoints
    payload_available_for_use_map[dest_pure_rank]->MarkNotReady(); // threadsafe

    // wait until it's ok to go (until any in-progress Dequeue is happening)
    // note: this code duplicated below in the condition variable wait section

    // extra lock for this first attempt at inserted into
    // requested_messages_metadata is needed as C++'s standard libraries are not
    // thread-safe for writes. The write inside the condition variable shouldn't
    // need an extra lock because it has it's own other lock as a function of
    // the condition variable.

    {
        // TODO(jim): use emplace as it's meant to be used; pass the arguments
        // to "new MPIMessageMetadata" directly to emplace and it will construct
        // the MPIMessageMetadata object for me.
        std::unique_lock<std::mutex> recv_ready_for_me_lock(
                recv_ready_for_me_mutex);
        auto emplace_ret =
                requested_messages_metadata.emplace(dest_pure_rank, metadata);
        bool added_receiver =
                emplace_ret
                        .second; /* see
                                    http://www.cplusplus.com/reference/unordered_map/unordered_map/emplace/
                                    */
        if (!added_receiver) {
            // Wait for previous dequeue to finish if a dequeue is in flight
            recv_ready_for_me_cv.wait(recv_ready_for_me_lock, [this,
                                                               dest_pure_rank,
                                                               metadata]() {
                auto emplace_ret = requested_messages_metadata.emplace(
                        dest_pure_rank, metadata);

                bool added_receiver =
                        emplace_ret
                                .second; /* see
                                            http://www.cplusplus.com/reference/unordered_map/unordered_map/emplace/
                                            */

                return added_receiver;
            });
        }
    }
    /////////////////////////////////////////////////////////////////////////////////

    // START////////////////////////// CRITICAL SECTION
    // ///////////////////////////////////////////
    {

        // STEP 2: update the number of messages in this bundle

        std::unique_lock<std::mutex> add_requested_message_lock(
                add_requested_message_mutex);
        ++curr_num_messages_in_bundle;
        if (curr_num_messages_in_bundle > total_num_messages_in_bundle) {
            fprintf(stderr,
                    "ERROR: curr_num_messages_in_bundle (%d) > "
                    "total_num_messages_in_bundle (%d)\n",
                    curr_num_messages_in_bundle, total_num_messages_in_bundle);
        }
        assert(curr_num_messages_in_bundle <= total_num_messages_in_bundle);

        if (curr_num_messages_in_bundle == total_num_messages_in_bundle) {
            // STEP 3: when all requestors for this bundle have submitted their
            // message request, receive message from the network

            // all messages for this bundle have been requested, so go ahead and
            // receive the bundled message from the network using MPI

            if (PURE_DEBUG) {
                std::cerr << "total_num_messages_in_bundle achieved ("
                          << total_num_messages_in_bundle << "). MPI Recving..."
                          << std::endl;

                // MPI Recv into temporary buffer, and then parse message out
                // and put it into caller's specified buffer
                fprintf(stderr,
                        "[pure rank %d] Dequeue doing MPI Recv: (%p, %d, %d, "
                        "%d, %d)\n",
                        dest_pure_rank, bundled_message->BufPointerStart(),
                        bundled_message->TotalBufSizeBytes(),
                        static_cast<unsigned int>(BUNDLE_CHANNEL_MPI_DATATYPE),
                        endpoint_mpi_rank, mpi_message_tag);
            }

            int        mpi_recv_ret;
            MPI_Status status;
            mpi_recv_ret =
                    MPI_Recv(bundled_message->BufPointerStart(),
                             bundled_message->TotalBufSizeBytes(),
                             BUNDLE_CHANNEL_MPI_DATATYPE, endpoint_mpi_rank,
                             mpi_message_tag, MPI_COMM_WORLD, &status);
            assert(mpi_recv_ret == MPI_SUCCESS);

            // now the recv has been done and the bundled message needs to be
            // distributed to the correct receive buffers in the corresponding
            // threads.
            char* curr_bundle_buf_ptr =
                    static_cast<char*>(bundled_message->BufPointerStart());
            assert(requested_messages_metadata.size() ==
                   total_num_messages_in_bundle);

            for (auto i = 0; i < total_num_messages_in_bundle; ++i) {

                int          message_count;
                int          message_count_bytes;
                unsigned int message_datatype_uint;
                int          message_dest;
                int          message_tag;
                int          message_sender;
                int          message_seq_id;

                if (PURE_DEBUG) {
                    fprintf(stderr,
                            KGRN "ABOUT TO PARSE NEW PACKET AT "
                                 "curr_bundle_buf_ptr %p:\n",
                            curr_bundle_buf_ptr);
                    std::cerr << BundledMessage::ToString(curr_bundle_buf_ptr)
                              << std::endl
                              << KRESET;
                }

                MPIMessageMetadata::Parse(curr_bundle_buf_ptr, &message_count,
                                          &message_count_bytes,
                                          &message_datatype_uint, &message_dest,
                                          &message_tag, &message_sender,
                                          &message_seq_id);
                MPI::Datatype message_datatype =
                        static_cast<MPI::Datatype>(message_datatype_uint);

                // look up the requested_metadata for the package that just came
                // in. So, look up the metadata for message_dest (the pure rank
                // of the destination).
                MPIMessageMetadata* requested_metadata =
                        requested_messages_metadata[message_dest];

                if (PURE_DEBUG) {
                    fprintf(stderr,
                            "[rank %d, iteration %d] getting "
                            "requested_metadata (%p) for "
                            "dest %d.\n",
                            dest_pure_rank, i, requested_metadata,
                            message_dest);

                    fprintf(stderr,
                            "[thread %d] message for pure dest %d: packet "
                            "message_count=%d; receiver expected %d\n",
                            dest_pure_rank, message_dest, message_count,
                            requested_metadata->GetCount());
                }

                assert(message_count == requested_metadata->GetCount());
                assert(message_count_bytes ==
                       requested_metadata->GetCountBytes());
                assert(message_datatype == requested_metadata->GetDatatype());
                assert(message_sender == requested_metadata->GetPureSender());
                assert(message_dest == requested_metadata->GetPureDest());
                assert(message_tag == requested_metadata->GetTag());

                // advance pointer to start of payload of user message
                curr_bundle_buf_ptr += MPIMessageMetadata::MetadataBufLen;

                if (requested_metadata->GetBufferAllocationScheme() ==
                    PureRT::BufferAllocationScheme::user_buffer) {
                    // copy into user's buffer. slower, but more flexible.

                    if (PURE_DEBUG) {
                        fprintf(stderr,
                                "[iteration %d] memcpy, %d bytes,%p,%p,%d,%d\n",
                                i, message_count_bytes,
                                static_cast<void*>(curr_bundle_buf_ptr),
                                requested_metadata->GetOrigBuf(), message_dest,
                                dest_pure_rank);
                    }

                    memcpy(requested_metadata->GetOrigBuf(),
                           curr_bundle_buf_ptr, message_count_bytes);
                    payload_available_for_use_map.at(message_dest)
                            ->MarkReadyAndNotifyOne(); // threadsafe

                } else {

                    // TODO(jim): is there a way to get the class name or enum
                    // value from an enum class? throw std::invalid_argument(
                    // "Invalid buffer scheme argument: " +
                    // requested_metadata->GetBufferAllocationScheme() );

                    fprintf(stderr, "expected %d but got %d\n",
                            static_cast<int>(PureRT::BufferAllocationScheme::
                                                     user_buffer),
                            static_cast<int>(
                                    requested_metadata
                                            ->GetBufferAllocationScheme()));
                    throw std::invalid_argument(
                            "Invalid buffer scheme argument...");
                }

                curr_bundle_buf_ptr += message_count_bytes;

            } // for all message in the requested_messages_metadata map

            {
                std::unique_lock<std::mutex> recv_ready_for_me_lock(
                        recv_ready_for_me_mutex);

                // clear out the requested_messages_map for next time
                requested_messages_metadata.clear();
                curr_num_messages_in_bundle   = 0;
                message_received_from_network = true;
                ++num_bundled_messages_sent_or_received;
            }
            recv_ready_for_me_cv.notify_all();

        } // if(curr_num_messages_in_bundle == total_num_messages_in_bundle)
    }
    // END////////////////////////// CRITICAL SECTION
    // ///////////////////////////////////////////

    return message_received_from_network;
}

void BundleChannel::WaitTaggedDequeueTODO(int pure_thread_rank) const {

    // TODO(jim): refactor this and rip out initialized for use after the
    // Initalized routines are done being refactored
    assert(initialized_for_use || initializer_done_initializing_cf.Ready());

    // TODO(jim):   make sure wait can be called multiple times
    if (all_payloads_available_for_use_map.at(pure_thread_rank)
                ->payload_available_cv.Ready()) {
        return;
    }
    all_payloads_available_for_use_map.at(pure_thread_rank)
            ->payload_available_cv.WaitForReady();

} // ends function WaitTaggedDequeueTODO
#endif
// ends DEPRECATED

void BundleChannel::Wait(int pure_thread_rank) const {

    // TODO(jim): refactor this and rip out initialized for use after the
    // Initalized routines are done being refactored
    assert(initialized_for_use || initializer_done_initializing_cf.Ready());

    // TODO(jim):   make sure wait can be called multiple times

    if (payload_available_for_use_map.at(pure_thread_rank)->Ready()) {
#if PURE_DEBUG
        std::cerr << "Early exit from BundleChannel::Wait() without waiting on "
                     "cv."
                  << std::endl;
#endif
        return;
    }
    payload_available_for_use_map.at(pure_thread_rank)->WaitForReady();
}

// helper function to enable sorting in BundleChannel::ToString
bool BoundChannelEndpointSorter(BundleChannelEndpoint* bce0,
                                BundleChannelEndpoint* bce1) {
    return bce0->GetSenderPureRank() < bce1->GetSenderPureRank();
}

string BundleChannel::ToString() const {

    stringstream ss;
    string       type_string, type_verb;

    if (endpoint_type == PureRT::EndpointType::SENDER) {
        type_string = "send";
        type_verb   = "sent";
    } else if (endpoint_type == PureRT::EndpointType::RECEIVER) {
        type_string = "receive";
        type_verb   = "received";
    } else if (endpoint_type == PureRT::EndpointType::REDUCE) {
        type_string = "reduce";
        type_verb   = "reduced";
    } else {
        sentinel("Invalid type. This shouldn't happen.");
    }

    ss << "BundleChannel (" << type_string << ") at " << this << std::endl;
    ss << "  " << curr_num_messages_in_bundle << " msgs posted to Dequeue (of "
       << total_num_messages_in_bundle << " total)" << std::endl;
    ss << "  " << num_bundled_messages_sent_or_received << " bundles "
       << type_verb << " thus far" << std::endl;
    ss << "  " << num_bound_channel_endpoints
       << " bound channel endpoints thus far" << std::endl;

    // TODO(jim): create sorted copy of endpoint array; had trouble with this
    // deadlocking... copy constructor:
    // http://stackoverflow.com/questions/16137953/is-there-a-function-to-copy-an-array-in-c-c

    /*
    BundleChannelEndpoint **sorted_bound_channel_endpoints = new
    BundleChannelEndpoint*[num_bound_channel_endpoints];
    std::copy(bound_channel_endpoints, bound_channel_endpoints +
    num_bound_channel_endpoints, sorted_bound_channel_endpoints);
    sort(sorted_bound_channel_endpoints, sorted_bound_channel_endpoints +
    num_bound_channel_endpoints, BoundChannelEndpointSorter);
    */

    for (auto i = 0; i < num_bound_channel_endpoints; ++i) {

        // BundleChannelEndpoint *bce = sorted_bound_channel_endpoints[i];
        BundleChannelEndpoint* bce = bound_channel_endpoints[i];

        bce->CheckInvariants();

        ss << "====[endpoint " << i << "] ";
        ss << "CET: " << bce->GetChannelEndpointTag() << " ";

        // force the base class functions, which don't do extra checking and
        // therefore print nullptr
        ss << "send_buf: " << bce->BundleChannelEndpoint::GetSendBuf() << " ";
        ss << "recv_buf: " << bce->BundleChannelEndpoint::GetRecvBuf() << " ";
        ss << bce->GetSenderPureRank() << "->" << bce->GetDestPureRank() << " ";
        ss << "dt: " << bce->GetDatatype() << " ";
        ss << "cnt: " << bce->GetCount() << " ";
        ss << "tag: " << bce->GetTag() << " ";
        ss << std::endl;

        bce->CheckInvariants();
    }

    // delete[] sorted_bound_channel_endpoints;

    return ss.str();
}

void BundleChannel::Initialize(int thread_num_in_mpi_process) {

    /* Algorithm:
     *   The goal is to determine the buffer size for the bundle channel and
     * then allocate the bundle channel to the proper size. Note that each
     * message in a given bundle can have a different payload size. The payload
     * size is determined by a combination of the datatype and the count of the
     * number of payload entries.
     *
     *   thread 0 is the initializer. it sets up the BundleChannel and then
     * signals the other threads that are waiting for the initialization
     *
     *   non-initializer threads: threads 1 - (num_threads-1). wait until
     * BundleChannel was initialized, and then return. the non-initializers
     * simply have to wait until the BundleChannel was set up.
     *
     */

    assert(endpoint_type == PureRT::EndpointType::SENDER ||
           endpoint_type == PureRT::EndpointType::RECEIVER);

    {
        // let the first thread here determine who is the initializer
        std::lock_guard<std::mutex> determine_initializer_lock(
                determine_initializer_mutex);

        if (initializer_thread_num < 0) {
            initializer_thread_num = thread_num_in_mpi_process;
        }
    } // unlocks

    // must barrier here to make sure all of the other send channels have
    // reached this point. so we can accurately determine the leader.
    pthread_barrier_wait(&wait_for_all_binders_complete_barrier);

#if PURE_DEBUG
    sleep_random_seconds(159);
    std::cerr << "Thread " << initializer_thread_num
              << " will initilize BundledMessage" << std::endl;
#endif

    // initializer thread
    if (thread_num_in_mpi_process == initializer_thread_num) {

        if (num_bound_channel_endpoints != total_num_messages_in_bundle) {
            std::cerr << "ERROR: num_bound_channel_endpoints ("
                      << num_bound_channel_endpoints
                      << ") != total_num_messages_in_bundle ("
                      << total_num_messages_in_bundle << ")" << std::endl;
        }
        assert(num_bound_channel_endpoints == total_num_messages_in_bundle);

        if (endpoint_type == PureRT::EndpointType::RECEIVER) {

            // initialize data structure for dequeues
            for (auto i = 0; i < num_bound_channel_endpoints; ++i) {
                payload_available_for_use_map.insert(std::make_pair(
                        bound_channel_endpoints[i]->GetDestPureRank(),
                        new ConditionFrame()));
            }
        }

        // TODO(jim): only deal with setting up extra data structures if this is
        // a sender. receivers still need the space for the metadata (as it does
        // come on the wire from MPI_Recv), but these initializations of
        // addresss aren't necessary
        bundled_message =
                new BundledMessage(endpoint_type, total_num_messages_in_bundle,
                                   bound_channel_endpoints);

        // TODO(jim): replace this with a ConditionFrame
        {
            std::unique_lock<std::mutex> wait_for_initialization_lock(
                    wait_for_initialization_mutex);
            initialized_for_use = true;
        }
        wait_for_initialization_cv.notify_all();

    } else {
        // non-initilizer

        std::unique_lock<std::mutex> wait_for_initialization_lock(
                wait_for_initialization_mutex);
        wait_for_initialization_cv.wait(wait_for_initialization_lock, [this]() {
            return initialized_for_use;
        });
    }

#if PURE_DEBUG
    sleep_random_seconds(159);
    std::cerr << "Thread " << thread_num_in_mpi_process
              << ": bundled_message = " << bundled_message << std::endl;
#endif
}

#ifdef DEPRECATED
// thread-safe
void BundleChannel::BindChannelEndpoint(BundleChannelEndpoint* bce) {

    /*
     * Algorithm:
     *   The goal is to give this bundle channel a list of its associated
     * BundleChannelEndpoints for future reference (one of which is to be able
     * to allocate the right buffer size for its bundled message).
     */

    assert(this->endpoint_type == PureRT::EndpointType::SENDER ||
           this->endpoint_type == PureRT::EndpointType::RECEIVER);

#if PURE_DEBUG
    if (num_bound_channel_endpoints >= total_num_messages_in_bundle) {
        ts_debug("num_bound_channel_endpoints=%d", num_bound_channel_endpoints);
        ts_debug("total_num_messages_in_bundle=%d",
                 total_num_messages_in_bundle);
    }
#endif
    assert(EndpointsAvailable());
    {
        std::lock_guard<std::mutex> bound_channel_endpoints_lock(
                bound_channel_endpoints_mutex);
        ////////////////////// CRITICAL SECTION START
        ////////////////////////////////
        bound_channel_endpoints[num_bound_channel_endpoints] = bce;
        ++num_bound_channel_endpoints;
        ////////////////////// CRITICAL SECTION END
        //////////////////////////////////
    }

} // function BindChannelEndpoint
#endif

#ifdef OLD_CODE_DEP

bool BundleChannel::Enqueue(int sender_pure_rank) {

    // STEP: assert invariants
    // TODO(jim): refactor this and rip out initialized for use after the
    // Initalized routines are done being refactored
    assert(initialized_for_use || initializer_done_initializing_cf.Ready());
    assert(bundled_message != nullptr);
    assert(bundled_message);

    bool msg_sent = false;

    // STEP: wait until previous iteration Enqueue from me, if any, is complete
    // (similar to Dequeue's approach). probably use the set of senders
    // approach. probably remove this functionality from bundledmessage and put
    // it here, which is more appropriate place for it.
    // START////////////////////////// CRITICAL SECTION
    // ///////////////////////////////////////////
    {

        std::unique_lock<std::mutex> send_ready_for_me_lock(
                send_ready_for_me_mutex);
        auto insert_ret = ready_sender_ranks.insert(sender_pure_rank);

        bool added_sender = insert_ret.second;
        if (!added_sender) {
            // wait for previous enqueue to finish, as the above would have
            // failed if another Enqueue on this bundle channel is currently
            // happening and not yet finished.
            send_ready_for_me_cv.wait(
                    send_ready_for_me_lock, [this, sender_pure_rank]() {
                        auto insert_ret =
                                ready_sender_ranks.insert(sender_pure_rank);
                        bool added_sender = insert_ret.second;
                        return added_sender;
                    });
        }
    }
    // END//////////////////////////// CRITICAL SECTION
    // ///////////////////////////////////////////

    // START////////////////////////// CRITICAL SECTION
    // ///////////////////////////////////////////
    {

        // STEP 2: update the number of messages in this bundle

        std::unique_lock<std::mutex> add_send_message_lock(
                add_send_message_mutex);

        // TODO(jim): optimization: this entire section could be done with an
        // atomic curr_num_message_in_bundle and without a lock, possibly. Try
        // it.
        bundled_message->CopyMessageData(sender_pure_rank);

        ++curr_num_messages_in_bundle;
        assert(curr_num_messages_in_bundle <= total_num_messages_in_bundle);

        if (curr_num_messages_in_bundle == total_num_messages_in_bundle) {
            // at this point, all of the message data has been added, so send
            // the message.

            /*          ts_log_info(KCYN "[rank%d] BundleChannel Enqueue doing
               MPI_Send %d to process %d." KRESET, sender_pure_rank,
                                    (num_bundled_messages_sent_or_received+1),
                                    endpoint_mpi_rank);
            */

            /*
            // TODO: rip out after verifying:

                                    MPI::COMM_WORLD.Send(bundled_message->BufPointerStart(),
                                                             bundled_message->TotalBufSizeBytes(),
                                                             BUNDLE_CHANNEL_MPI_DATATYPE,
                                                             endpoint_mpi_rank,
                                                             mpi_message_tag);
            */
            MPI_Send(bundled_message->BufPointerStart(),
                     bundled_message->TotalBufSizeBytes(),
                     BUNDLE_CHANNEL_MPI_DATATYPE, endpoint_mpi_rank,
                     mpi_message_tag, MPI_COMM_WORLD);

            msg_sent                    = true;
            curr_num_messages_in_bundle = 0;
            ++num_bundled_messages_sent_or_received;

            {
                std::unique_lock<std::mutex> send_ready_for_me_lock(
                        send_ready_for_me_mutex);
                ready_sender_ranks.clear();
            }
            send_ready_for_me_cv.notify_all();

        } // ends if(curr_num_messages_in_bundle ==
          // total_num_messages_in_bundle)
        assert(curr_num_messages_in_bundle <= total_num_messages_in_bundle);
    }
    // END//////////////////////////// CRITICAL SECTION
    // ///////////////////////////////////////////

    return msg_sent;

} // ends Enqueue

#endif