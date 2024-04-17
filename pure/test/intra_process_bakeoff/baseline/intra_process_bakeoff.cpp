// Author: James Psota
// File:   intra_process_bakeoff.cpp 

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
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "mpi.h"
#include "pure/support/benchmark_timer.h"
#include "pure/support/helpers.h"
#include "pure/support/stats_generator.h"
#include <iomanip>

using namespace std;
using namespace PureRT;

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
              int payload_count) {

    assert(num_ranks % 2 == 0);
    assert(my_rank < num_ranks);

    const int msg_tag       = 5555;
    const int sender_rank   = 0;
    const int receiver_rank = 1;
    int*      buf = new int[payload_count]; // used for both senders (t0 and t1)

    // just initialize once in the sender and repeatedly use the buffer
    if (my_rank == sender_rank) {
        for (int i = 0; i < payload_count; ++i) {
            buf[i] = i;
        }

        for (auto i = 0; i < send_recv_iterations; ++i) {
            MPI_Request send_request;
            MPI_Status  send_status;
            int ret = MPI_Isend(buf, payload_count, MPI_INT, receiver_rank,
                                msg_tag, MPI_COMM_WORLD, &send_request);
            assert(ret == MPI_SUCCESS);
            MPI_Wait(&send_request, &send_status);
        }
    } else {
        for (auto i = 0; i < send_recv_iterations; ++i) {
            // receiver
            MPI_Request recv_request;
            MPI_Status  recv_status;
            int ret = MPI_Irecv(buf, payload_count, MPI_INT, sender_rank,
                                msg_tag, MPI_COMM_WORLD, &recv_request);
            assert(ret == MPI_SUCCESS);
            MPI_Wait(&recv_request, &recv_status);

            check_payload(buf, payload_count, i);
        }
    }

    delete[](buf);

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

    // this benchmark only works with these settings
    assert_nprocs(2);
    assert_num_threads(1);

    MPI_Init(&argc, &argv);
    int my_rank, num_ranks;
    MPI_Comm_rank(MPI_COMM_WORLD, &my_rank);
    MPI_Comm_size(MPI_COMM_WORLD, &num_ranks);

    // hack to remove
    /*
        void* max_tag_ptr;
        int   flag;
        int ret = MPI_Comm_get_attr(MPI_COMM_WORLD, MPI_TAG_UB, max_tag_ptr,
       &flag); assert(ret == MPI_SUCCESS);

        if (flag) {
            fprintf(stderr, "r%d: tag ub is %d  MPI_TAG_UB is %d\n", my_rank,
                    *(int*)max_tag_ptr, MPI_TAG_UB);
        } else {
            fprintf(stderr, "tag size failed\n");
            exit(-2);
        }
    */
    /////////////////////////////////////////////////

    BenchmarkTimer timer("End-to-End");
    timer.start();

    run_test(my_rank, num_ranks, send_recv_iterations, payload_count);

    timer.pause();

    MPI_Finalize();

    string stats = PureRT::generate_stats(my_rank, timer,
                                          std::vector<BenchmarkTimer*>());

    const int my_threads_cpu = PureRT::get_cpu_on_linux();
    fprintf(stderr,
            KGRN "Rank %d (on CPU %d) returned from MPI_Finalize. "
                 "SUCCESS.\n" KRESET,
            my_rank, my_threads_cpu);
    fprintf(stderr, "[r%d] %s\n", my_rank, stats.c_str());
    if (my_rank == 0) {
        fprintf(stderr, "num_iterations: %d; buf count: %d\n\n",
                send_recv_iterations, payload_count);
    }

    exit(0);
}