// Author: James Psota
// File:   reduce_bundle_channel.cpp 

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

#include "pure/transport/reduce_bundle_channel.h"
#include "pure/transport/bundled_message.h"

#define PURE_MAX_ROOT_RANK 1024

ReduceBundleChannel::ReduceBundleChannel(int root_mpi_rank_arg,
                                         int total_num_messages_in_bundle_arg,
                                         MPI_Op mpi_op_arg,
                                         int    num_processes_arg)
    : BundleChannel(root_mpi_rank_arg, PureRT::EndpointType::REDUCE,
                    total_num_messages_in_bundle_arg),
      reduction_send_buffer(nullptr), mpi_reduce_receive_buf(nullptr),
      root_mpi_rank(root_mpi_rank_arg), mpi_op(mpi_op_arg) {

    check((root_mpi_rank >= 0 && root_mpi_rank < PURE_MAX_ROOT_RANK),
          "root_mpi_rank (%d) must be reasonable", root_mpi_rank);

    payload_available_cfs = new ConditionFrame[total_num_messages_in_bundle];
    multi_process         = (num_processes_arg > 1);
    multi_threaded        = (total_num_messages_in_bundle > 1);

    // TODO(jim): verify that it's ok to use all threads in a process. Right now
    // I'm assuming
    // MPI_COMM_WORLD is always used, so all threads in all processes would
    // participate in the
    // reduction.
} // constructor

ReduceBundleChannel::~ReduceBundleChannel() {

    // The destructor of the base class (BundleChannel) is automatically called
    // in reverse order of
    // construction.
    // (http://stackoverflow.com/questions/677620/do-i-need-to-explicitly-call-the-base-virtual-destructor)
    //    BundleChannel::~BundleChannel();
    //~BundleChannel();
    delete[](static_cast<char*>(reduction_send_buffer));
    delete[](static_cast<char*>(mpi_reduce_receive_buf));
    delete thread_send_bundled_message;
    delete[](payload_available_cfs);
} // dconstructor

void ReduceBundleChannel::BindChannelEndpoint(BundleChannelEndpoint* bce,
                                              int                    mpi_rank) {
    check(!BundleChannel::InitializationDone(),
          "The base class bundle_channel must not yet be initialized, but it is. This may mean that you're trying to create multiple reduce bundle channels with the same CET.");
    check((mpi_rank >= 0 && mpi_rank < PURE_MAX_ROOT_RANK) ||
                  mpi_rank == PureRT::PURE_UNUSED_RANK,
          "mpi_rank (%d) must be reasonable", mpi_rank);
    check((root_mpi_rank >= 0 && root_mpi_rank < PURE_MAX_ROOT_RANK),
          "root_mpi_rank (%d) must be reasonable", root_mpi_rank);
    ts_log_info(
            "Before BundleChannel::BindChannelEndpointNewTODO. mpi_rank = %d",
            mpi_rank);
    // first, call the base class version of this function. For this case, this
    // basically just
    // adds in the bce pointer to the BundleChannels' bce array and blocks the
    // threads until every
    // other thread is done (at which point InitializationDone() returns true).

    bool all_endpoints_bound_by_me =
            BundleChannel::BaseBindChannelEndpoint(bce, true);
    ts_log_info("After BundleChannel::BindChannelEndpointNewTODO");
    ts_log_info("mpi_rank: %d; root_mpi_rank: %d", mpi_rank, root_mpi_rank);

    // TODO(jim): overview of this section
    // http://www.cplusplus.com/reference/atomic/atomic/compare_exchange_strong/
    if (all_endpoints_bound_by_me) {

// determine which of the bound receive endpoints have the receiver buffer, if
// any. There
// should be one or zero bound channel endpoints that have a non-nullptr
// recv_buffer
// it should be one if this is the root_mpi_rank, and zero otherwise.
#if DEBUG_CHECK
        for (auto i = 0; i < total_num_messages_in_bundle; ++i) {
            check(num_bound_channel_endpoints == total_num_messages_in_bundle,
                  "Number of bound channel endpoints (%d) should be the same "
                  "as the total "
                  "num "
                  "messages in bundle (%d).",
                  num_bound_channel_endpoints, total_num_messages_in_bundle);
            check(bce->NumPayloadBytes() ==
                          bound_channel_endpoints[i]->NumPayloadBytes(),
                  "Payload bytes for first bundle channel endpoint (%d) should "
                  "be the same "
                  "as "
                  "payload bytes "
                  "for endpoint with index %d (%d)",
                  bce->NumPayloadBytes(), i,
                  bound_channel_endpoints[i]->NumPayloadBytes());
            check(bce->GetCount() == bound_channel_endpoints[i]->GetCount(),
                  "Count for first bundle channel endpoint (%d) should be the "
                  "same as "
                  "count "
                  "for endpoint with index %d (%d)",
                  count, i, bound_channel_endpoints[i]->GetCount());
            check(bce->GetDatatype() ==
                          bound_channel_endpoints[i]->GetDatatype(),
                  "datatype for first bundle channel endpoint (%d) should be "
                  "the same as "
                  "datatype "
                  "for endpoint with index %d (%d)",
                  datatype, i, bound_channel_endpoints[i]->GetDatatype());
        }
#endif
        // given the above check, we are assured that all BCEs have the same
        // count and datetype size. So,
        // just use this bce to calculate buf_size_bytes.
        buf_size_bytes = bce->GetCount() * bce->GetDatatypeBytes();

        // at this point, on the root node, all of the bundle channel endpoints
        // have been
        // bound to the bundle
        // channel
        // only one should have a non-null receive buffer. look for it, and save
        // it to the
        // mpi_reduce_receive_buf pointer
        if (mpi_rank == root_mpi_rank) {

            int root_index_uninit = -1;
            int root_index        = root_index_uninit;
            for (auto q = 0; q < total_num_messages_in_bundle; ++q) {

                // note that bound_channel_endpoints[q]->GetDestPureRank has the
                // rank that
                // is the root rank. We use the insight that the root endpoint
                // is the one
                // where the sender and receiver are the same.

                if (bound_channel_endpoints[q]->GetDestPureRank() ==
                    bound_channel_endpoints[q]->GetSenderPureRank()) {
                    if (root_index == root_index_uninit) {
                        // hasn't been set yet, so we're good. just save the
                        // index
                        root_index = q;
                    } else {
                        sentinel("Found multiple bound_channel_endpoints "
                                 "(including ranks "
                                 "%d and %d, but there may be others) where "
                                 "the sender and "
                                 "destination ranks are the same. However, "
                                 "this should "
                                 "only happen for the root pure "
                                 "rank",
                                 bound_channel_endpoints[root_index]
                                         ->GetSenderPureRank(),
                                 bound_channel_endpoints[q]
                                         ->GetSenderPureRank());
                    }
                } else {
                    ts_log_info(KYEL "bound_channel_endpoints[%d]->GetRecvBuf()"
                                     " = %p" KRESET,
                                q, bound_channel_endpoints[q]->GetRecvBuf());
                }
            }
            if (root_index == root_index_uninit) {
                sentinel("Failed to find bound_channel_endpoints that is the "
                         "root rank "
                         "(test: sender and receiver rank are the same)");
            }

            // at this point, root_index points to the single
            // bound_channel_endpoints index
            // that has a non-nullptr recv buf
            // TODO(jim): verify, then rip out: mpi_reduce_receive_buf =
            // bound_channel_endpoints[root_index]->GetRecvBuf();
            mpi_reduce_receive_buf = new char[buf_size_bytes];
            // set the runtime channel receive buffer of the root rank.
            // this is where the final answer will go.
            bound_channel_endpoints[root_index]->SetRecvChannelBuf(
                    mpi_reduce_receive_buf);
            ts_log_info(
                    "== Set recv channel buf of rank %d to %p",
                    bound_channel_endpoints[root_index]->GetSenderPureRank(),
                    mpi_reduce_receive_buf);
        } // ends if this one of the threads in the root_mpi_rank process (any
          // of which
          // could be the initializing, not necessarily the root pure rank)

        // create the buffer that is used to do the intra-process reduction, and
        // also used
        // to enact the MPI_Reduce. This is the "send buffer" for this process.
        reduction_send_buffer = new char[buf_size_bytes];
        ts_log_info(
                KGRN
                "BindChannelEndpoint: reduction_send_buffer set to: %p" KRESET,
                reduction_send_buffer);

        check(reduction_send_buffer != nullptr,
              "reduction_send_buffer wasn't allocated properly. address is %p",
              reduction_send_buffer);

        datatype = bce->GetDatatype();
        count    = bce->GetCount();

        // note: a side effect of the BundledMessage constructor is to set the
        // runtime send
        // buffer on each bound channel endpoint object.
        thread_send_bundled_message = new BundledMessage(
                PureRT::EndpointType::REDUCE, total_num_messages_in_bundle,
                bound_channel_endpoints);

        SignalAllThreadsInitialized(total_num_messages_in_bundle);
        // Note: not needed as they will be waiting on the base class
        // WaitForInitialization, and the
        // rbc initialized comes after this. In fact, I think teh
        // rbc_initialized structure can be
        // removed.
        // rbc_initialized_cv.notify_all();

    } // if all channel endpoints have been bound (i.e.,
      // all_endpoints_bound_by_me)

} // function BindChannelEndpointNewTODO

// essentially called by the user via a BundleChannelEndpoint in the user code
void ReduceBundleChannel::WaitForInitialization() const {

    ts_log_info(
            KGRN
            "BEFORE WaitForInitialization: reduction_send_buffer: %p" KRESET,
            reduction_send_buffer);
    BundleChannel::WaitForInitialization();
    ts_log_info(KGRN
                "AFTER WaitForInitialization: reduction_send_buffer: %p" KRESET,
                reduction_send_buffer);

    // note: TSAN found a data race here because we're using double check
    // locking
    // if this doesn't solve the problem, two options: 1) find some way to
    // instrument TSAN; 2) just
    // do the check inside a lock

    /*
        // optimization opportunity. this will always be set to true after it's
       set once.
        {
            std::unique_lock<std::mutex>
       rbc_initialization_lock(rbc_initialization_mutex);
            if (!rbc_initialized) {
                rbc_initialized_cv.wait(rbc_initialization_lock, [this]() {
       return rbc_initialized;
       });
            }
        } // ends critical section based on rbc_initialization mutex
    */
    // TODO(jim): perhaps here, we set up the send buffer and recv buffer for
    // the root, so that
    // the user is able to get them. The guarantee is that they are ready upon
    // completing the call
    // to WaitForInitialization.

    // Or, better yet, do this in the reduce_channel.cpp (at the thread level)

} // function WaitForInitialization

void ReduceBundleChannel::Wait(int thread_num_in_mpi_process) const {

    // TODO(jim): refactor this and rip out initialized for use after the
    // Initalized routines are
    // done being refactored

    // TODO(jim): I think Wait should only be called by the root. If that's the
    // case the
    check(BundleChannel::InitializationDone(), "BundleChannel initialization "
                                               "must be done before calling "
                                               "ReduceBundleChannel::Wait()");
    BundleChannel::AssertInvarients();

    if (multi_threaded) {
        if (payload_available_cfs[thread_num_in_mpi_process].Ready()) {
            return;
        }
        payload_available_cfs[thread_num_in_mpi_process].WaitForReady();
    }

} // function Wait

/////////////////////////////////////////////////////////
// TEMPLATE SPECIALIZATIONS
// I don't love this, but doing it to try to get symbols and line numbers in
// stack traces
// TODO(jim): is this necessary? remove these if possible.
template <>
bool ReduceBundleChannel::Reduce(int, int*,
                                 int); // specialization for SendBufType = int
template <>
bool ReduceBundleChannel::Reduce(int, float*,
                                 int); // specialization for SendBufType = float
template <>
bool ReduceBundleChannel::Reduce(int, double*, int); // specialization for
                                                     // SendBufType = double
template <>
bool ReduceBundleChannel::Reduce(int, long*, int); // specialization for
                                                   // SendBufType = long
template <>
bool ReduceBundleChannel::Reduce(int, uint64_t*, int); // specialization for
                                                       // SendBufType = uint64_t
template <>
bool ReduceBundleChannel::Reduce(int, char*,
                                 int); // specialization for SendBufType = char
