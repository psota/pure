// Author: James Psota
// File:   reduce_bundle_channel.h 

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

/*
    optimization todo:
      don't make all the non-roots wait until the entire computation is done.
   they should be able to
   proceed right after their buffer is done being incorporated. Just make sure
   they don't go onto
   the next iteration using standard mechanisms.

    don't lock num_messages in Reduce if not multi-threaded.
 */

#ifndef PURE_TRANSPORT_REDUCE_BUNDLE_CHANNEL_H
#define PURE_TRANSPORT_REDUCE_BUNDLE_CHANNEL_H

#pragma once

#include <type_traits>

#include "pure/support/zed_debug.h"
#include "pure/transport/bundle_channel.h"
#include "pure/transport/bundle_channel_endpoint.h"

class BundleChannel; // forward declaration

class ReduceBundleChannel : public BundleChannel {

  private:
    // TODO: fixme.
    // char padding_debug_todo_ripout[40];

    void*           reduction_send_buffer;
    void*           mpi_reduce_receive_buf;
    BundledMessage* thread_send_bundled_message{};

    const int    root_mpi_rank;
    int          count{};
    MPI_Datatype datatype{};
    const MPI_Op mpi_op;
    bool         multi_process;
    bool         multi_threaded;
    int          buf_size_bytes;

    bool reduction_send_buffer_initialized = false;

    // array of condition frames, one per thread
    ConditionFrame* payload_available_cfs;

  public:
    ReduceBundleChannel(int /*root_mpi_rank_arg*/,
                        int /*total_num_messages_in_bundle_arg*/,
                        MPI_Op /*mpi_op_arg*/, int /* num_processes */);
    ~ReduceBundleChannel() override;

    void BindChannelEndpoint(BundleChannelEndpoint* /*bce*/, int /*mpi_rank*/);
    void WaitForInitialization() const;
    void Wait(int /* thread_num_in_mpi_process */) const;
    //////////////////////////////////////////////////////////////////////////////////////

    // have to define template functions in header file
    // (http://stackoverflow.com/questions/1639797/template-issue-causes-linker-error-c)
    template <typename SendBufType>
    bool Reduce(int sender_pure_rank, SendBufType* __restrict thread_send_buf,
                int thread_num_in_mpi_process) {

// STEP 1: Check invariants and wait until ready to go (previous iterations
// finished, etc.)
#if DEBUG_CHECK
        ts_log_info(KGRN "STARTING Reduce: reduction_send_buffer: %p" KRESET,
                    reduction_send_buffer);
        for (int p = 0; p < num_bound_channel_endpoints; ++p) {
            assert(sizeof(SendBufType) ==
                   bound_channel_endpoints[p]->GetDatatypeBytes());
        }
        AssertInvarients();
#endif

        if (multi_threaded) {
            // reset payload available flag for this iteration to false only for
            // "me" --
            // thread_num_in_mpi_process
            if (payload_available_cfs[thread_num_in_mpi_process].Ready()) {
                // this was set from the last Reduce of this ReduceChannel.
                // Reset it.
                payload_available_cfs[thread_num_in_mpi_process].MarkNotReady();
            }

            WaitAllThreadsReady(sender_pure_rank);
        }
        check(BundleChannel::InitializationDone(),
              "underlying bundle channel must be initialized (via the "
              "initializer_done_initializing_cf ConditionFrame) before "
              "calling Reduce.");
        check(reduction_send_buffer != nullptr, "reduction_send_buffer should "
                                                "not be null by the time "
                                                "Reduce() is called");
        bool mpi_reduce_enacted = false;
        bool last_thread_in;

        // START////////////////////////// CRITICAL SECTION
        {
            // STEP 2: update the number of messages in this bundle
            // N.B. num_messages_in_bundle_mutex is also the defacto lock around
            // updating the
            // reduction_send_buffer
            std::unique_lock<std::mutex> num_messages_in_bundle_lock(
                    num_messages_in_bundle_mutex);
            ++curr_num_messages_in_bundle;
            last_thread_in = (curr_num_messages_in_bundle ==
                              total_num_messages_in_bundle);

            check(curr_num_messages_in_bundle <= total_num_messages_in_bundle,
                  "curr_num_messages_in_bundle (%d) must be less than or equal "
                  "to "
                  "total_num_messages_in_bundle (%d)",
                  curr_num_messages_in_bundle, total_num_messages_in_bundle);

            // STEP 3: reduce in this thread's data into the process-level
            // reduction send buffer.
            // if necessary, initialize the reduction array
            // initialize the buffer with the initialization value

            // cast reduction buffer to get the array indexing to work
            SendBufType* __restrict typed_reduction_send_buffer =
                    static_cast<SendBufType*>(reduction_send_buffer);
            SendBufType prev_val;

            switch (mpi_op) {
            case MPI_SUM:

                for (int i = 0; i < count; ++i) {
                    if (!reduction_send_buffer_initialized) {
                        // note: we already verified above (with DEBUG_CHECK on)
                        // that all
                        // bound_channel_endpoints have the
                        // same datatype (and datatype bytes), so we just take
                        // the first one here.
                        memset(typed_reduction_send_buffer, 0, buf_size_bytes);
                        assert(buf_size_bytes ==
                               bound_channel_endpoints[0]->NumPayloadBytes());
                        reduction_send_buffer_initialized = true;
                    }
                    // TODO(jim): remove this value after done debugging
                    typed_reduction_send_buffer[i] += thread_send_buf[i];

                } // ends for all elements in array
                break;

            case MPI_PROD:

                for (int i = 0; i < count; ++i) {

                    if (!reduction_send_buffer_initialized) {
                        for (int h = 0; h < count; ++h) {
                            typed_reduction_send_buffer[h] = 1;
                        }
                        reduction_send_buffer_initialized = true;
                    }
                    typed_reduction_send_buffer[i] *= thread_send_buf[i];

                } // ends for all elements in array
                break;
            case MPI_MAX:
                if (reduction_send_buffer_initialized) {
                    for (auto i = 0; i < count; ++i) {
                        if (thread_send_buf[i] >
                            typed_reduction_send_buffer[i]) {
                            // if this thread's element is greater than the
                            // current one, update it. otherwise, do nothing.
                            typed_reduction_send_buffer[i] = thread_send_buf[i];
                        }
                    }
                } else {
                    // just copy this thread's buffer in to get started
                    std::memcpy(typed_reduction_send_buffer, thread_send_buf,
                                buf_size_bytes);
                    reduction_send_buffer_initialized = true;
                }
                break;
            case MPI_MIN:
                if (reduction_send_buffer_initialized) {
                    for (auto i = 0; i < count; ++i) {
                        if (thread_send_buf[i] <
                            typed_reduction_send_buffer[i]) {
                            // if this thread's element is greater than the
                            // current one, update it. otherwise, do nothing.
                            typed_reduction_send_buffer[i] = thread_send_buf[i];
                        }
                    }
                } else {
                    // just copy this thread's buffer in to get started
                    std::memcpy(typed_reduction_send_buffer, thread_send_buf,
                                buf_size_bytes);
                    reduction_send_buffer_initialized = true;
                }
                break;
            default:
                sentinel("Unsupported MPI_Op (%d). This is either a bug or "
                         "not yet implemented. "
                         "http://thy.phy.bnl.gov/~creutz/qcdoc/mpi/mpi.h "
                         "may be helpful.",
                         static_cast<int>(mpi_op));
            }

            assert(curr_num_messages_in_bundle <= total_num_messages_in_bundle);

        } // ends critical section

        // STEP 4: if this is the last thread from this iteration to reduce,
        // do the
        // process-level
        // reduction using MPI_Reduce
        if (last_thread_in) { ////
            // at this point, all of the message data has been added, so do
            // the process-level
            // reduction

            // note that the MPI_Reduce is invoked by the *last* thread to
            // enter this section.
            // This has
            // nothing to do with which thread happens to be the root. At
            // this point, after the
            // RBC has
            // been initialized, the reduce buffers have been set, so any
            // thread can enact the
            // MPI_Reduce.

            /*
            for (int u = 0; u < count; ++u) {
                if(std::is_floating_point<SendBufType >::value) {
                     ts_log_info("[r%d] BEFORE MPI_Reduce:
            reduction_send_buffer[%d] = %lf",
                                sender_pure_rank, u,
            typed_reduction_send_buffer[u]);

                } else if(std::is_integral<SendBufType >::value) {
                    ts_log_info("[r%d] BEFORE MPI_Reduce:
            reduction_send_buffer[%d] = %d",
                                sender_pure_rank, u,
            typed_reduction_send_buffer[u]);
                } else {
                    sentinel("Unsupported type (only currently support
            integers and floats)");
                }
            }
            */

            assert(reduction_send_buffer != nullptr);
            if (multi_process) {
                // in the multiprocess case, we do the MPI_Reduce to do the
                // final reduction into
                // mpi_reduce_receive_buf
                // on the root
                int reduce_return = MPI_Reduce(
                        reduction_send_buffer, mpi_reduce_receive_buf, count,
                        datatype, mpi_op, root_mpi_rank, MPI_COMM_WORLD);
                assert(reduce_return == MPI_SUCCESS);
                mpi_reduce_enacted = true;
            } else {
                // in the single process case, we must manually copy the
                // contents of the now already-reduced
                // reduction_send_buffer
                // to the mpi_reduce_receive_buffer
                std::memcpy(mpi_reduce_receive_buf, reduction_send_buffer,
                            buf_size_bytes);
            }

            if (multi_threaded) {
                // The reduction has completed. Signal to all waiters that
                // they can now proceed.
                for (auto t = 0; t < total_num_messages_in_bundle; ++t) {
                    if (t == thread_num_in_mpi_process) {
                        // this is me, so just mark ready. no need to notify
                        // on the condition
                        // variable, as this thread is here executing this
                        // code and therefore can't
                        // be in Wait()
                        payload_available_cfs[t].MarkReady();
                    } else {
                        payload_available_cfs[t].MarkReadyAndNotifyOne();
                    }
                }
            }

            reduction_send_buffer_initialized =
                    false; // reset for next use of this ReduceBundleChannel
                           // object
            ResetState();

            if (multi_threaded) {
                SignalAllThreadsReady(); // note:
                                         // num_messages_in_bundle_mutex
                                         // must be held to call
                                         // this function (it is, above)
            }

        } // ends if(last_thread)

        // END//////////////////////////// CRITICAL SECTION
        return mpi_reduce_enacted;

    } // function Reduce

}; // class

#endif