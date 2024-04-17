// Author: James Psota
// File:   intra_process_bakeoff_pure.cpp 

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


#include "assert.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "mpi.h"
#include "pure.h"
#include "pure/support/benchmark_timer.h"
#include "pure/support/stats_generator.h"
#include <iomanip>

void check_payload(int* recv_buf, int payload_count, int send_recv_iteration) {
#if DEBUG_CHECK
    for (int i = 0; i < payload_count; ++i) {
        if (recv_buf[i] != i) {
            fprintf(stderr,
                    "Expected %d but got %d (index: %d, "
                    "send_recv_iteration: %d)\n",
                    i, recv_buf[i], i, send_recv_iteration);
            std::abort();
        }
    }
#endif
} // ends function check_payload

void run_test(int my_rank, int num_ranks, int send_recv_iterations,
              int payload_count, PureThread* pure_thread,
              BenchmarkTimer& timer) {

    assert(num_ranks % 2 == 0);
    assert(my_rank < num_ranks);

    const int msg_tag       = 400;
    const int sender_rank   = 0;
    const int receiver_rank = 1;

    SendChannel* sc = nullptr;
    RecvChannel* rc = nullptr;

    const MPI_Datatype datatype = MPI_INT;

    // just initialize once in the sender and repeatedly use the buffer
    bool do_recv_free = false;

    if (my_rank == sender_rank) {
        sc = pure_thread->InitSendChannel(nullptr, payload_count, datatype,
                                          receiver_rank, msg_tag);
        // buf = sc->GetSendChannelBuf<int*>();
        auto* const __restrict buf =
                jp_memory_alloc<1, CACHE_LINE_BYTES, false, int>(payload_count *
                                                                 sizeof(int));

        for (int i = 0; i < payload_count; ++i) {
            buf[i] = i;
        }

        timer.start();
        for (auto i = 0; i < send_recv_iterations; ++i) {
            sc->EnqueueBlocking();
        }
        timer.pause();
    } else {
        const auto using_rt_recv_buf =
                payload_count * 4 < BUFFERED_CHAN_MAX_PAYLOAD_BYTES;
        rc = pure_thread->InitRecvChannel(payload_count, datatype, sender_rank,
                                          msg_tag, using_rt_recv_buf);
        timer.start();

        if (using_rt_recv_buf) {
            // use RT buffer
            do_recv_free = false;
            for (auto i = 0; i < send_recv_iterations; ++i) {
                rc->DequeueBlocking();
#if DEBUG_CHECK
                int* recv_buf = rc->GetRecvChannelBuf<int*>();
                check_payload(recv_buf, payload_count, i);
#endif
            }
        } else {

            // int recv_buf[payload_count];
            auto* const __restrict recv_buf = static_cast<int*>(
                    jp_memory_alloc<1>(payload_count * sizeof(int)));
            do_recv_free = true;
            for (auto i = 0; i < send_recv_iterations; ++i) {
                // use user buffer
                rc->DequeueBlocking(recv_buf);
#if DEBUG_CHECK
                check_payload(recv_buf, payload_count, i);
#endif
            }
        }

        timer.pause();

        // eh leak memory!
        // if (do_recv_free) {
        //     free(recv_buf);
        // }
    }

} // ends function run_test

int main(int argc, char* argv[]) {

    int send_recv_iterations, payload_count;

    if (argc < 3) {
        fprintf(stderr,
                KRED "ERROR: usage: %s <num_iterations> <payload_count>" KRESET,
                argv[0]);
        exit(-1);
    } else {
        send_recv_iterations = atoi(argv[1]);
        assert(send_recv_iterations > 0);
        payload_count = atoi(argv[2]);
        assert(payload_count > 0);
        fprintf(stderr, "Running with: num_iterations: %d; buf count: %d\n",
                send_recv_iterations, payload_count);
    }

    // this benchmark only works with two total threads
    const int num_procs   = atoi(getenv("PURE_NUM_PROCS"));
    const int num_threads = atoi(getenv("PURE_RT_NUM_THREADS"));
    assert(num_procs > 0);
    assert(num_threads > 0);
    assert(num_procs * num_threads == 2);

    MPI::Init(argc, argv);
    int my_rank   = MPI::COMM_WORLD.Get_rank();
    int num_ranks = MPI::COMM_WORLD.Get_size();

    BenchmarkTimer timer("End-to-End");
    run_test(my_rank, num_ranks, send_recv_iterations, payload_count,
             pure_thread, timer);

    MPI::Finalize();
    pure_thread_global->Finalize();

    std::vector<BenchmarkTimer*> user_custom_timers; // empty
    string stats = PureRT::generate_stats(my_rank, timer, user_custom_timers,
                                          pure_thread->RuntimeTimers());

    const int my_threads_cpu = PureRT::get_cpu_on_linux();
    fprintf(stderr,
            KGRN "Rank %d (on CPU %d) returned from MPI::Finalize. "
                 "SUCCESS.\n" KRESET,
            my_rank, my_threads_cpu);
    fprintf(stderr, "[r%d] %s\n", my_rank, stats.c_str());
#if COLLECT_THREAD_TIMELINE_DETAIL
    TimerIntervalDetailsToCsv(my_rank, timer, user_custom_timers,
                              pure_thread->RuntimeTimers());
#endif

    exit(0);
}