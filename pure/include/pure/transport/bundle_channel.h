// Author: James Psota
// File:   bundle_channel.h 

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

#ifndef PURE_TRANSPORT_BUNDLE_CHANNEL_H
#define PURE_TRANSPORT_BUNDLE_CHANNEL_H

#pragma once

#include "mpi.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <climits>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>

#include "pure/3rd_party/pthread/pthread_barrier_apple.h"
#include "pure/common/pure_rt_enums.h"
#include "pure/support/condition_frame.h"
#include "pure/support/helpers.h"

// TODO(jim): make this a static const class member as best practice
#define BUNDLE_CHANNEL_MPI_DATATYPE MPI_CHAR
#define BUNDLE_CHANNEL_UNUSED_TAG (-68000)
#define BUNDLE_CHANNEL_UNUSED_NUM_MESSAGES (-68001)

using std::ostream;
using std::string;
using std::vector;

// forward declaration
class SendChannel;
class RecvChannel;
class BundleChannelEndpoint;
class BundledMessage;
class MPIMessageMetadata;

/*
TODO: remove
typedef struct _ReceiverThreadParsedTracker {

    int total_num_messages_in_bundle_for_me =
            1; // start at one, and increment if needed upon initialization
    int            num_messages_parsed_this_time = 0;
    ConditionFrame payload_available_cv;

} ReceiverThreadParsedTracker;
*/

class BundleChannel {

  public:
    BundleChannel(int /*endpoint_mpi_rank_arg*/,
                  PureRT::EndpointType /*endpoint_type_arg*/,
                  int total_num_messages_in_bundle_arg =
                          BUNDLE_CHANNEL_UNUSED_NUM_MESSAGES,
                  int mpi_message_tag_arg = BUNDLE_CHANNEL_UNUSED_TAG);
    virtual ~BundleChannel();

    // DEPRECATED: void BindChannelEndpoint(BundleChannelEndpoint* /*bce*/);
    // virtual void BindChannelEndpoint(BundleChannelEndpoint* /*bce*/) = 0; //
    // pure virtual; must be overridden by subclasses.

    void WaitForInitialization() const;

    void Initialize(int /*thread_num_in_mpi_process*/); // refactor / integrate
                                                        // with above

    inline int CurrNumMessages() { return curr_num_messages_in_bundle; }
    inline int TotalNumMessages() { return total_num_messages_in_bundle; }
    inline int NumBundledMessagesSent() {
        return num_bundled_messages_sent_or_received;
    }

    inline bool EndpointsAvailable() {
        return (num_bound_channel_endpoints < total_num_messages_in_bundle);
    }
    inline bool EndpointsNotAvailable() { return !EndpointsAvailable(); }
    // virtual bool Enqueue(int) = 0; /* TODO: should this be pure virtual? pure
    // virtual function - must be defined in child class */
    string                      ToString() const;
    inline PureRT::EndpointType GetEndpointType() const {
        return endpoint_type;
    }

// TODO(jim): remove this
#ifdef DEPRECATED
    // TODO(jim): Make dequeue pure vitual
    bool Dequeue(void* const /*buf*/, int /*count*/,
                 const MPI::Datatype& /*datatype*/, int /*sender_pure_rank*/,
                 int /*tag*/, int /*dest_pure_rank*/,
                 RecvChannel* /* recv_channel */);
    bool TaggedDequeueTODO(int /*count*/, const MPI::Datatype& /*datatype*/,
                           int /*sender_pure_rank*/, int /*tag*/,
                           int /*dest_pure_rank*/,
                           RecvChannel* /* recv_channel */);
    // TODO(jim): remove this
    void WaitTaggedDequeueTODO(int /*pure_thread_rank*/) const;
#endif

    void Wait(int /*pure_thread_rank*/) const;

  protected:
    /* NEW VARAIBLES FOR REDUCE REFACTOR!!! */
    // TODO(jim): re-order member vars

    int                  curr_num_messages_in_bundle;
    int                  total_num_messages_in_bundle;
    PureRT::EndpointType endpoint_type;

    BundledMessage* bundled_message;
    int             endpoint_mpi_rank;
    int             mpi_message_tag;

    mutable std::mutex              op_ready_for_me_mutex;
    mutable std::condition_variable op_ready_for_me_cv;
    mutable std::mutex              num_messages_in_bundle_mutex;
    std::set<int>                   ready_thread_ranks;

    void AssertInvarients() const;
    void ResetState();
    void WaitAllThreadsReady(int /*caller_pure_thread_rank*/);
    void SignalAllThreadsReady();
    void SignalAllThreadsInitialized(int num_threads = -1);

    std::string ReadyThreadRanksToString() const;

    bool BaseBindChannelEndpoint(BundleChannelEndpoint* /*bce*/,
                                 bool /*more_child_initialization*/);

    inline bool InitializationDone() const {
        return initializer_done_initializing_cf.Ready();
    }

    int                     num_bound_channel_endpoints;
    BundleChannelEndpoint** bound_channel_endpoints; // array of pointers to
                                                     // channel endpoints

    /* END NEW VARAIBLES FOR REDUCE REFACTOR!!! */

  private:
    mutable std::mutex      bound_channel_endpoints_mutex;
    static std::atomic<int> num_local_bundle_channels;

    // OLD STUFF BELOW. Clean up, move around, etc.

    inline static int MakeReqMsgsKeys(int sender_pure_rank,
                                      int dest_pure_rank) {
        assert(sender_pure_rank < (1 << 15)); // double check that this scheme
                                              // works. only would break down
                                              // for very large runs...
        assert(dest_pure_rank < (1 << 15));
        return (sender_pure_rank << 16) | dest_pure_rank;
    }

    int num_bundled_messages_sent_or_received;

    //////////////////////// INITIALIZATION ////////////////////////
    ConditionFrame initializer_done_initializing_cf;

    mutable std::mutex bound_recv_channels_mutex;

    mutable std::mutex determine_initializer_mutex;
    int                initializer_thread_num;

    pthread_barrier_t wait_for_all_binders_complete_barrier;

    mutable std::mutex      wait_for_initialization_mutex;
    bool                    initialized_for_use;
    std::condition_variable wait_for_initialization_cv;

    //////////////////////////////SENDER/////////////////////

    //////////////////////////////RECEIVER///////////////////
    mutable std::mutex add_requested_message_mutex;

    // to deal with Dequeues in a loop
    mutable std::mutex      recv_ready_for_me_mutex;
    std::condition_variable recv_ready_for_me_cv;

    // For receiving purposes, keep an array of in-flight requested
    // MPIMessageMetadatas. Use these when
    // enough messages have come in to actually do the receive. Only valid for
    // receive channels.
    // the key is the pure rank of the receiver (one message per thread) or the
    // combination of the sender
    // and receiver ranks (tagged bundle
    // channels).
    std::unordered_map<int, MPIMessageMetadata*> requested_messages_metadata;

    // Condition that Wait uses to know if a payload is available for use
    std::unordered_map<int, ConditionFrame*> payload_available_for_use_map;

    // TODO(jim): TODO (refactor) Tagged Bundle Channel Payload Available for
    // use data structure

    /* This data structure keeps track of when it's ok for a thread to return
       from Wait(). The thread that actually receives the data signals to all
       the other threads when their messages have been received. However, note
       that each thread can receive multiple messages. Therefore, a given thread
       should only be signaled (to wake up from Wait) when *all* of its messages
       have been received. We are assuming here that the user code only calls
       wait once after *all* of its Dequeues have been issued.

       This data structure keeps track of how many messages each thread expects
       to receive (for each bundle), and how many have been received so far.

     */

    // TODO(jim): remove: std::unordered_map<int, ReceiverThreadParsedTracker*>
    // all_payloads_available_for_use_map;
};
#endif
