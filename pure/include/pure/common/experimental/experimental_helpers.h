// Author: James Psota
// File:   experimental_helpers.h 

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

#ifndef EXPERIMENTAL_HELPERS_H
#define EXPERIMENTAL_HELPERS_H

#include "pure/transport/all_reduce_channel.h"
#include "pure/transport/scan_channel.h"

// WARNING: this channel is created once, used once, and then thrown away. So,
// if you want to use it multiple times, don't do it this way.
template <typename ValType>
void bcast_single_val(PureThread* pure_thread, int my_rank_in_comm,
                      ValType* user_buf, MPI_Datatype dt, unsigned int cet,
                      int count = 1, int root_rank_in_comm = 0,
                      PureComm* const pc = nullptr) {

    //  MPI_Bcast(&order, 1, MPI_LONG, root, MPI_COMM_WORLD);
    // const bool is_root = (my_rank == root);
    ValType* buf;

    BcastChannel* bcast_channel = pure_thread->InitBcastChannel(
            count, dt, root_rank_in_comm, cet, pc);

    // if (is_root) {
    //     buf    = bcast_channel->GetSendChannelBuf<ValType*>();
    //     buf[0] = *user_buf; // write value to be broadcast
    // } else {
    //     buf = bcast_channel->GetRecvChannelBuf<ValType*>();
    // }

    // thread 1 (non root) is here
    bcast_channel->Bcast(user_buf); // blocks until everyone has their value
                                    // locally
    // if (!is_root) {
    //     *user_buf = buf[0];
    // }
}

// WARNING: this channel is created once, used once, and then thrown away. So,
// if you want to use it multiple times, don't do it this way.
template <typename ValType>
void reduce_single_value(PureThread* pure_thread, int my_rank_in_comm,
                         ValType* local_user_buf, ValType* result_user_buf,
                         MPI_Datatype dt, MPI_Op mpi_op, unsigned int cet,
                         PureComm* const pc = nullptr) {

    const int count             = 1;
    const int root_rank_in_comm = 0;
    // const bool is_root   = (my_rank == root_rank);

    ReduceChannel* channel = pure_thread->InitReduceChannel(
            count, dt, root_rank_in_comm, mpi_op, cet, pc);
    channel->WaitForInitialization();
    channel->Reduce(local_user_buf, result_user_buf);
}

static const thread_local int printed = 0;
// IMPORTANT: keep the if/else trees and logic above and below consistent
inline void pure_send_recv(PureThread* pure_thread, SendChannel* sc,
                           RecvChannel* rc) {
    assert(pure_thread->Get_rank() == sc->GetSenderPureRank());
    assert(pure_thread->Get_rank() == rc->GetDestPureRank());

    const auto dest_rank_for_outgoing_msg = sc->GetDestPureRank();
    const auto send_rank_for_incoming_msg = rc->GetSenderPureRank();

    if (send_rank_for_incoming_msg == dest_rank_for_outgoing_msg) {
        // Scheme 1: potential deadlock situation where two ranks are both
        // sending to and receiving from each other

        if (sc->GetSenderPureRank() < sc->GetDestPureRank()) {
            // equivalent to: my_rank < dest_rank. send first.

            // if (printed == 0) {
            //     // fprintf(stderr, "[r%d] pure_send_recv: E then D\n",
            //     // sc->GetSenderPureRank());
            //     printed = 1;
            // }

            sc->Enqueue();
            rc->Dequeue();
        } else {
            // equivalent to: my_rank >= dest_rank. receive first.
            // if (printed == 0) {
            //     // fprintf(stderr, "[r%d] pure_send_recv: D then E\n",
            //     // sc->GetSenderPureRank());
            //     printed = 1;
            // }

            rc->Dequeue();
            sc->Enqueue();
        }
    } else if (send_rank_for_incoming_msg < dest_rank_for_outgoing_msg) {
        // Scheme 2: incoming message is coming from a rank that is different
        // from the rank that the outgoing meessage is going to.
        // we want to do the communication (send or receive) with the lower
        // remote rank to send first
        sc->Enqueue();
        rc->Dequeue();
    } else {
        // recv first
        rc->Dequeue();
        sc->Enqueue();
    }
}

// IMPORTANT: keep the if/else trees and logic above and below consistent
inline void pure_send_recv_user_bufs(SendChannel* const sc,
                                     const buffer_t     send_user_buf,
                                     size_t send_count, RecvChannel* const rc,
                                     const buffer_t recv_user_buf) {
    assert(pure_thread_global->Get_rank() == sc->GetSenderPureRank());
    assert(pure_thread_global->Get_rank() == rc->GetDestPureRank());

    // these are Pure ranks
    const auto dest_rank_for_outgoing_msg = sc->GetDestPureRank();
    const auto send_rank_for_incoming_msg = rc->GetSenderPureRank();

    // we need to implement send-recv manually because we are basically mixing
    // and matching pure and mpi sends and receives. we generally like to post a
    // receive first so we can place the value directly into the receiver's
    // desired buffer.

    const auto my_pure_rank = pure_thread_global->Get_rank();
    const auto uses_mpi_for_send =
            (my_pure_rank != pure_thread_global->MPIRankFromPureRank(
                                     dest_rank_for_outgoing_msg));
    const auto uses_mpi_for_recv =
            (my_pure_rank != pure_thread_global->MPIRankFromPureRank(
                                     send_rank_for_incoming_msg));

    const auto print_rank = false;

    if (send_rank_for_incoming_msg == dest_rank_for_outgoing_msg) {
        // Scheme 1: potential deadlock situation where two ranks are both
        // sending to and receiving from each other
        if (sc->GetSenderPureRank() < sc->GetDestPureRank()) {
            // equivalent to: my_rank < dest_rank. send first.

            if (uses_mpi_for_recv) {
                // then we force the receive to be first, like MPI_Sendrecv does
                rc->Dequeue(recv_user_buf);
                sc->EnqueueUserBuf(send_user_buf, send_count);
            } else {
                sc->EnqueueUserBuf(send_user_buf, send_count);
                if (print_rank)
                    fprintf(stderr, "    [r%d] enqueue done\n", PURE_RANK);
                rc->Dequeue(recv_user_buf);

                if (print_rank)
                    fprintf(stderr, "    [r%d] deq done\n", PURE_RANK);
                // sc->Wait();
                // rc->Wait();
            }

        } else {
            // equivalent to: my_rank >= dest_rank. receive first.

            if (print_rank) {
                fprintf(stderr,
                        "[r%d]\t SCENARIO 1b: RECEIVING: %d -> (%d)  then   "
                        "SENDING: (%d) ->  %d"
                        "\n",
                        PURE_RANK, send_rank_for_incoming_msg, PURE_RANK,
                        PURE_RANK, dest_rank_for_outgoing_msg);
            }

            rc->Dequeue(recv_user_buf);
            if (print_rank)
                fprintf(stderr, "    [r%d] deq done\n", PURE_RANK);
            sc->EnqueueUserBuf(send_user_buf, send_count);
            // rc->Wait();
            // sc->Wait();
            if (print_rank)
                fprintf(stderr, "    [r%d] enqueue done\n", PURE_RANK);
        }

    } else if (send_rank_for_incoming_msg < dest_rank_for_outgoing_msg) {
        // Scheme 2: incoming message is coming from a rank that is different
        // from the rank that the outgoing meessage is going to.
        // we want to do the communication (send or receive) with the lower
        // remote rank to send first

        if (print_rank) {
            fprintf(stderr,
                    "[r%d]\t SCENARIO 2: SENDING: (%d) ->  %d  then  "
                    "RECEIVING: %d -> (%d)"
                    "\n",
                    PURE_RANK, PURE_RANK, dest_rank_for_outgoing_msg,
                    send_rank_for_incoming_msg, PURE_RANK);
        }

        if (uses_mpi_for_recv) {
            rc->Dequeue(recv_user_buf);
            sc->EnqueueUserBuf(send_user_buf, send_count);
            if (print_rank)
                fprintf(stderr, "    [r%d] enqueue done\n", PURE_RANK);

            // sc->Wait();
            // rc->Wait();
        } else {
            sc->EnqueueUserBuf(send_user_buf, send_count);
            if (print_rank)
                fprintf(stderr, "    [r%d] enqueue done\n", PURE_RANK);

            rc->Dequeue(recv_user_buf);
            // sc->Wait();
            // rc->Wait();
        }

        if (print_rank)
            fprintf(stderr, "    [r%d] deq done\n", PURE_RANK);

    } else {

        if (print_rank) {
            fprintf(stderr,
                    "[r%d]\t SCENARIO 3: RECEIVING: %d -> (%d)  then   "
                    "SENDING: (%d) ->  %d"
                    "\n",
                    PURE_RANK, send_rank_for_incoming_msg, PURE_RANK, PURE_RANK,
                    dest_rank_for_outgoing_msg);
        }

        // recv first
        rc->Dequeue(recv_user_buf);
        if (print_rank)
            fprintf(stderr, "    [r%d] deq done\n", PURE_RANK);
        sc->EnqueueUserBuf(send_user_buf, send_count);
        // rc->Wait();
        // sc->Wait();
        if (print_rank)
            fprintf(stderr, "    [r%d] enqueue done\n", PURE_RANK);
    }
}

// we keep looping and trying to receive from sc and rc in either order using
// the "test" facility
inline void pure_wait_sc_rc_any_order(SendChannel* const sc,
                                      RecvChannel* const rc) {

    bool send_done = false, recv_done = false;

    while ((send_done && recv_done) == false) {
        if (send_done == false) {
            if (recv_done) {
                // we only have to complete the send now, so just wait instead
                // of test
                sc->Wait();
                send_done = true;
            } else {
                send_done = sc->Test();
            }
        }
        if (recv_done == false) {
            if (send_done) {
                rc->Wait();
                recv_done = true;
            } else {
                recv_done = rc->Test();
            }
        }
    }

    assert(send_done == true);
    assert(recv_done == true);
}

// IMPORTANT: keep the if/else trees and logic above and below consistent
// Feb 2024 debugging version
inline void pure_send_recv_user_bufs_with_waits(SendChannel* const sc,
                                                const buffer_t send_user_buf,
                                                size_t         send_count,
                                                RecvChannel* const rc,
                                                const buffer_t recv_user_buf) {
    assert(pure_thread_global->Get_rank() == sc->GetSenderPureRank());
    assert(pure_thread_global->Get_rank() == rc->GetDestPureRank());
    const auto dest_rank_for_outgoing_msg = sc->GetDestPureRank();
    const auto send_rank_for_incoming_msg = rc->GetSenderPureRank();

    // const auto print_rank = PURE_RANK == 0 || PURE_RANK == 1;
    const auto print_rank = false;

    if (send_rank_for_incoming_msg == dest_rank_for_outgoing_msg) {
        // Scheme 1: potential deadlock situation where two ranks are both
        // sending to and receiving from each other
        if (sc->GetSenderPureRank() < sc->GetDestPureRank()) {
            // equivalent to: my_rank < dest_rank. send first.

            if (print_rank) {
                fprintf(stderr,
                        "[r%d]\t SCENARIO 1a: SENDING: (%d) ->  %d  then  "
                        "RECEIVING: %d -> (%d)"
                        "\n",
                        PURE_RANK, PURE_RANK, dest_rank_for_outgoing_msg,
                        send_rank_for_incoming_msg, PURE_RANK);
            }

            sc->EnqueueUserBuf(send_user_buf, send_count);
            if (print_rank)
                fprintf(stderr, "    [r%d] enqueue done\n", PURE_RANK);
            rc->Dequeue(recv_user_buf);

            sc->Wait();
            rc->Wait();
            if (print_rank)
                fprintf(stderr, "    [r%d] deq done\n", PURE_RANK);

        } else {
            // equivalent to: my_rank >= dest_rank. receive first.

            if (print_rank) {
                fprintf(stderr,
                        "[r%d]\t SCENARIO 1b: RECEIVING: %d -> (%d)  then   "
                        "SENDING: (%d) ->  %d"
                        "\n",
                        PURE_RANK, send_rank_for_incoming_msg, PURE_RANK,
                        PURE_RANK, dest_rank_for_outgoing_msg);
            }

            rc->Dequeue(recv_user_buf);
            if (print_rank)
                fprintf(stderr, "    [r%d] deq done\n", PURE_RANK);
            sc->EnqueueUserBuf(send_user_buf, send_count);
            rc->Wait();
            sc->Wait();
            if (print_rank)
                fprintf(stderr, "    [r%d] enqueue done\n", PURE_RANK);
        }

    } else if (send_rank_for_incoming_msg < dest_rank_for_outgoing_msg) {

        if (print_rank) {
            fprintf(stderr,
                    "[r%d]\t SCENARIO 2: SENDING: (%d) ->  %d  then  "
                    "RECEIVING: %d -> (%d)"
                    "\n",
                    PURE_RANK, PURE_RANK, dest_rank_for_outgoing_msg,
                    send_rank_for_incoming_msg, PURE_RANK);
        }

        // Scheme 2: incoming message is coming from a rank that is different
        // from the rank that the outgoing meessage is going to.
        // we want to do the communication (send or receive) with the lower
        // remote rank to send first
        sc->EnqueueUserBuf(send_user_buf, send_count);
        if (print_rank)
            fprintf(stderr, "    [r%d] enqueue done\n", PURE_RANK);

        rc->Dequeue(recv_user_buf);
        sc->Wait();
        rc->Wait();

        if (print_rank)
            fprintf(stderr, "    [r%d] deq done\n", PURE_RANK);

    } else {

        if (print_rank) {
            fprintf(stderr,
                    "[r%d]\t SCENARIO 3: RECEIVING: %d -> (%d)  then   "
                    "SENDING: (%d) ->  %d"
                    "\n",
                    PURE_RANK, send_rank_for_incoming_msg, PURE_RANK, PURE_RANK,
                    dest_rank_for_outgoing_msg);
        }

        // recv first
        rc->Dequeue(recv_user_buf);
        if (print_rank)
            fprintf(stderr, "    [r%d] deq done\n", PURE_RANK);
        sc->EnqueueUserBuf(send_user_buf, send_count);
        rc->Wait();
        sc->Wait();
        if (print_rank)
            fprintf(stderr, "    [r%d] enqueue done\n", PURE_RANK);
    }
}

// N.B. https://mpi.deino.net/mpi_functions/MPI_Alltoall.html
template <typename ValType>
void all_to_all_managed(ValType const* const send_buf, int send_count,
                        MPI_Datatype send_dt, ValType* const recv_buf,
                        int recv_count, MPI_Datatype recv_dt,
                        int unique_chan_tag, int num_ranks) {

    // MPI_Request reqs[num_ranks];
    RecvChannel* recv_chans[num_ranks];

    for (auto i = 0; i < num_ranks; i++) {
        // const auto ret =
        //         MPI_Irecv(&recv_buf[i * recv_count], recv_count, recv_dt, i,
        //                   unique_chan_tag, MPI_COMM_WORLD, &reqs[i]);
        // assert(ret == MPI_SUCCESS);
        recv_chans[i] = pure_thread_global->GetOrInitRecvChannel(
                recv_count, recv_dt, i, unique_chan_tag, false,
                PROCESS_CHANNEL_BUFFERED_MSG_SIZE, PURE_COMM_WORLD);
        recv_chans[i]->Dequeue(&recv_buf[i * recv_count]);
    }

    for (auto i = 0; i < num_ranks; i++) {
        // const auto ret = MPI_Send(&send_buf[i * send_count], send_count,
        //                           send_dt, i, unique_chan_tag,
        //                           MPI_COMM_WORLD);
        // assert(ret == MPI_SUCCESS);
        const bool         using_rt_send_buf   = false;
        const bool         using_sized_enqueue = false;
        SendChannel* const sc = pure_thread_global->GetOrInitSendChannel(
                send_count, send_dt, i, unique_chan_tag, using_rt_send_buf,
                using_sized_enqueue, PROCESS_CHANNEL_BUFFERED_MSG_SIZE,
                PURE_COMM_WORLD);
        sc->EnqueueUserBuf(const_cast<int*>(&send_buf[i * send_count]));
        sc->Wait();
    }

    for (auto i = 0; i < num_ranks; i++) {
        // const auto ret = MPI_Wait(&reqs[i], MPI_STATUS_IGNORE);
        // assert(ret == MPI_SUCCESS);
        recv_chans[i]->Wait();
    }
}

#endif