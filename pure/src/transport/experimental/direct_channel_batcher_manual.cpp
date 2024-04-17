// Author: James Psota
// File:   direct_channel_batcher_manual.cpp 

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

#include "pure/transport/experimental/direct_channel_batcher_manual.h"
#include "pure.h"
#include "pure/runtime/pure_process.h"
#include "pure/transport/direct_mpi_channel.h"

#define DEBUG_PRINT 0

using namespace PureRT;

DirectChannelBatcherManual::DirectChannelBatcherManual(
        int num_parts, EndpointType endpoint_type, int count_per_part,
        MPI_Datatype datatype, PureThread* calling_pure_thread,
        int calling_pure_rank, int mate_pure_rank, int bytes_per_datatype_arg,
        int tag)
    : num_parts(num_parts), bytes_per_datatype(bytes_per_datatype_arg),
      agg_buf(malloc(num_parts * count_per_part * bytes_per_datatype_arg)),
      endpoint_type(endpoint_type), total_count(count_per_part * num_parts),
      datatype(datatype), tag(tag),
      payload_bytes(bytes_per_datatype_arg * count_per_part) {

    check(calling_pure_thread->GetThreadNumInProcess() == leader_thread_num,
          "Calling thread is %d but expected leader num %d",
          calling_pure_thread->GetThreadNumInProcess(), leader_thread_num);

    // Try to get this working via PureThread or PureProcess -- this is sort
    // of a mess
    SendChannel* sc;
    RecvChannel* rc;
    const bool   using_rt_send_buf_hint    = false;
    const bool   using_sized_enqueues_hint = false;
    const bool   using_rt_recv_buf         = false;

    // by definition?
    const int sender_thread_num   = leader_thread_num;
    const int receiver_thread_num = leader_thread_num;

    done_flags = new PaddedDoneFlag[num_parts];

    if (DEBUG_PRINT)
        fprintf(stderr, "r%d tag is %d\n", PURE_RANK, tag);

    if (endpoint_type == EndpointType::SENDER) {
        sc = new SendChannel(nullptr, total_count, datatype, calling_pure_rank,
                             tag, mate_pure_rank, CHANNEL_ENDPOINT_TAG_DEFAULT,
                             calling_pure_thread->GetTraceCommOutstream(),
                             using_rt_send_buf_hint, calling_pure_thread);
        endpoint = sc;
    } else {
        assert(endpoint_type == EndpointType::RECEIVER);
        rc = new RecvChannel(nullptr, total_count, datatype, mate_pure_rank,
                             tag, calling_pure_rank,
                             CHANNEL_ENDPOINT_TAG_DEFAULT,
                             calling_pure_thread->GetTraceCommOutstream(),
                             nullptr, calling_pure_thread);
        endpoint = rc;
    }

    assert(calling_pure_thread->MPIRankFromPureRank(
                   calling_pure_thread->Get_rank()) !=
           calling_pure_thread->MPIRankFromPureRank(mate_pure_rank));

    const int mate_mpi_rank =
            calling_pure_thread->MPIRankFromPureRank(mate_pure_rank);

    sentinel("This is most likely not working as intended given that it wasn't "
             "properly converted to use PureComms. Not fixing for now.");
    dmc = new DirectMPIChannel(endpoint, mate_mpi_rank, MPI_COMM_WORLD,
                               sender_thread_num, receiver_thread_num,
                               endpoint_type, using_rt_send_buf_hint,
                               using_sized_enqueues_hint, using_rt_recv_buf,
                               std::move(PurePipeline(0)));
    endpoint->SetDirectMPIChannel(dmc);
}

DirectChannelBatcherManual::~DirectChannelBatcherManual() {
    free(agg_buf);
    delete[] done_flags;
    delete dmc;

    if (endpoint_type == EndpointType::SENDER) {
        delete dynamic_cast<SendChannel*>(endpoint);
    } else {
        delete dynamic_cast<RecvChannel*>(endpoint);
    }
}

// TODO: assert that all threads have same parameters

void* DirectChannelBatcherManual::getBuf(int thread_num) {
    return reinterpret_cast<unsigned char*>(agg_buf) +
           (thread_num * payload_bytes);
}

//
void DirectChannelBatcherManual::Enqueue(int thread_num, bool& thread_sense) {
    // make sure it's a send channel
    assert(endpoint_type == EndpointType::SENDER);

    if (DEBUG_PRINT)
        fprintf(stderr, "r%d  Enqueue after waiting for prev\n", PURE_RANK);

    if (thread_num == leader_thread_num) {
        // 2. Wait until all threads have called enqueue (for this
        // iteration)

        if (DEBUG_PRINT)
            fprintf(stderr, "r%d  Enqueue leader start\n", PURE_RANK);

        while (1) {
            bool all_parts_done = true;
            // part 0 is done, as we already know
            for (auto i = 1; i < num_parts; ++i) {
                if (done_flags[i].done_flag.load(std::memory_order_acquire) !=
                    init_comm_done_sense) {
                    if (DEBUG_PRINT)
                        fprintf(stderr,
                                "   enq leader: some threads not done yet\n");
                    // usleep(500000);
                    all_parts_done = false;
                    break;
                }
            }

            if (all_parts_done) {
                // ok, all parts are done, so send the message

                if (DEBUG_PRINT)
                    fprintf(stderr,
                            "r%d  Enqueue leader all threads done calling "
                            "enqueue\n",
                            PURE_RANK);
                dmc->EnqueueUserBuf(agg_buf, total_count);

                // 4. invert the done sense for next time
                init_comm_done_sense.store(!init_comm_done_sense,
                                           std::memory_order_release);

                if (DEBUG_PRINT)
                    fprintf(stderr, "r%d  Enqueue leader - set senes to %d\n",
                            PURE_RANK, init_comm_done_sense.load());

                break; // break out of while loop
            }
            // otherwise, keep waiting for all pieces to be done
        } // while
    } else {
        // not a leader
        // 0. wait until the previous iteration was done
        // init_comm_done_sense starts at 0 as done thread sense
        while (thread_sense !=
               init_comm_done_sense.load(std::memory_order_acquire))
            // spin wait
            ;

        // 1. mark my part as done

        // TODO: change this to thread_sense!!!!!!!!!!!!!!  ??
        done_flags[thread_num].done_flag.store(thread_sense,
                                               std::memory_order_release);

        // flip the thread sense for next time
        thread_sense = !thread_sense;

        if (DEBUG_PRINT)
            fprintf(stderr,
                    "r%d  Enqueue set done flag[%d] to %d and set my thread "
                    "sense "
                    "to %d\n",
                    PURE_RANK, thread_num,
                    done_flags[thread_num].done_flag.load(), thread_sense);
    }
}

void DirectChannelBatcherManual::EnqueueWait(int   thread_num,
                                             bool& thread_sense) {

    // here each thread should wait until their message has been sent, just
    // like they were waiting on their own single message (to preserve
    // semantics)

    if (thread_num == leader_thread_num) {
        // leader
        dmc->EnqueueWait();
        wait_comm_done_sense.store(!wait_comm_done_sense,
                                   std::memory_order_release);
    } else {
        // not leader
        // wait until leader finishes the wait call
        while (thread_sense !=
               wait_comm_done_sense.load(std::memory_order_acquire))
            ;

        // set this for next time
        thread_sense = !thread_sense;
    }
}

void DirectChannelBatcherManual::Dequeue(int thread_num, bool& thread_sense) {
    assert(endpoint_type == EndpointType::RECEIVER);

    if (thread_num == leader_thread_num) {
        while (1) {
            bool buf_ok_to_overwrite = true;
            // part 0 is done because I'm here, as we already know
            assert(leader_thread_num == 0);
            for (auto i = 1; i < num_parts; ++i) {
                if (done_flags[i].done_flag.load(std::memory_order_acquire) !=
                    init_comm_done_sense) {
                    if (DEBUG_PRINT)
                        fprintf(stderr,
                                "   deq leader: some threads not done yet\n");
                    // usleep(500000);
                    buf_ok_to_overwrite = false;
                    break;
                }
            }

            if (buf_ok_to_overwrite) {
                // ok, buffer has been consumed by everyone, so receive the
                // message
                // TODO: double buffer this so we can use a different buffer
                // next time

                if (DEBUG_PRINT)
                    fprintf(stderr,
                            "r%d  Deq leader all threads done calling "
                            "dequeue\n",
                            PURE_RANK);
                dmc->Dequeue(agg_buf);

                // 4. invert the done sense for next time
                init_comm_done_sense.store(!init_comm_done_sense,
                                           std::memory_order_release);

                if (DEBUG_PRINT)
                    fprintf(stderr, "r%d  Deq leader - set senes to %d\n",
                            PURE_RANK, init_comm_done_sense.load());

                break; // break out of while loop
            }
        }
    } else {
        // non-leader
        // 1. mark my part as done

        // confused -- do I need to wait here again for the previous iteration ?

        done_flags[thread_num].done_flag.store(thread_sense,
                                               std::memory_order_release);
        thread_sense = !thread_sense;
    }
}

//// !!!! unify this with EnqueueWait?????
void DirectChannelBatcherManual::DequeueWait(int   thread_num,
                                             bool& thread_sense) {

    if (DEBUG_PRINT)
        fprintf(stderr, KCYN "[r%d] Dequeue wait begin \n" KRESET, PURE_RANK);

    // basically the leader needs to wait on the incomign message, and the
    // non-leaders need to wait until the leader indicates that the wait is
    // done
    if (thread_num == leader_thread_num) {
        dmc->DequeueWait();
        wait_comm_done_sense.store(!wait_comm_done_sense,
                                   std::memory_order_release);

        if (DEBUG_PRINT)
            fprintf(stderr, KCYN "[r%d] Dequeue wait done for leader \n" KRESET,
                    PURE_RANK);

    } else {
        // non-leader
        while (thread_sense !=
               wait_comm_done_sense.load(std::memory_order_acquire))
            ;

        // set this for next time
        thread_sense = !thread_sense;

        if (DEBUG_PRINT)
            fprintf(stderr,
                    KCYN "[r%d] Dequeue wait done for nonleader \n" KRESET,
                    PURE_RANK);
    }
}
