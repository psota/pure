// Author: James Psota
// File:   rand_jacobi_pure.cpp 

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
#include <string>

#include "pure.h"
#include "pure/common/experimental/experimental_helpers.h"
#include "pure/support/benchmark_timer.h"
#include "pure/support/stats_generator.h"

using namespace std;

const int iter_weight = ITER_WEIGHT;

double f(double input, int rank, int num_ranks) {
    double s = 0.0f;
#if DO_COMPUTATION
    // for (auto i = 0; i < iter_weight * rank; ++i) {
    //     s += i;
    // }

    // march 6 hack -- more imbalance
    int weight;
    if (rank >= num_ranks * 0.74) {
        weight = 10;
    } else {
        weight = 1;
    }

    for (auto i = 0; i < iter_weight * weight; ++i) {
        s += i;
    }
#endif
    return s;
}

void bench(int iter, int cls_per_rank, int my_rank, int n_ranks,
           BenchmarkTimer& t) {

    assert(iter > 1);

    int num_doubles = CACHE_LINE_BYTES / sizeof(double) * cls_per_rank;
    assert(num_doubles > 0);
    // double       a[n_per], temp[n_per];
    double *a, *temp;
    a    = (double*)jp_memory_alloc<1>(num_doubles * sizeof(double));
    temp = (double*)jp_memory_alloc<1>(num_doubles * sizeof(double));

    SendChannel *sc0, *sc1;
    RecvChannel *rc0, *rc1;

    // fill a with something deterministic
    for (auto i = 0; i < num_doubles; ++i) {
        a[i] = i;
    }

    PurePipeline f_task(1);

    for (auto it = 0; it < iter; ++it) {

        if (it == 1) {
            pure_thread_global->AllChannelsInit();
            PURE_BARRIER;
            t.start();
        }

#if PCV4_OPTION_EXECUTE_CONTINUATION
        if (it == 0) {
            auto                 owning_rank = PURE_RANK;
            VoidPureContinuation f_cont = [num_doubles, a, temp, owning_rank,
                                           n_ranks](void* recv_buf,
                                                    int   payload_count,
                                                    PureRTContExeMode m,
                                                    chunk_id_t start_chunk,
                                                    chunk_id_t end_chunk,
                                                    std::optional<void*>
                                                            cont_params) {
                pure_chunk_range_t range;
                if (m == PureRTContExeMode::CHUNK) {
                    range = PurePipeline::
                            PipelineStageCachelineAlignedIndexRange<double>(
                                    num_doubles, start_chunk, end_chunk);
                    assert_cacheline_aligned(&a[range.min_idx]);
                } else if (m == PureRTContExeMode::RECEIVER_FULL) {
                    const pure_chunk_range_index_t max = num_doubles - 1;
                    range                              = {0, max};
                } else {
                    sentinel("Invalid mode");
                }

                // PRINT_CONT_DEBUG_INFO("f_cont");
                for (auto i = range.min_idx; i <= range.max_idx; i++) {
                    temp[i] = f(a[i], owning_rank, n_ranks);
                }
            }; // end cont
            f_task.pipeline.emplace_back(std::move(f_cont), true);
            pure_thread_global->RegisterCollabPipeline(f_task);
        }
        f_task.Execute();
#else
        // turn this into a Pure Task
        for (auto i = 0; i < num_doubles; ++i) {
            // compute step
            temp[i] = f(a[i], my_rank, n_ranks);
        }
#endif

        // compute inner elements of a from temp
        for (auto i = 1; i < num_doubles - 1; ++i) {
            a[i] = (temp[i - 1] + temp[i] + temp[i + 1]) / 3.0;
        }

        if (n_ranks > 1) {
            if (my_rank > 0) {
                // send first element of my array to rank below me
                // MPI_Send(&temp[0], 1, MPI_DOUBLE, my_rank - 1, 0,
                // MPI_COMM_WORLD);
                const bool using_rt_send_buf   = false;
                const bool using_sized_enqueue = false;
                sc0 = pure_thread_global->GetOrInitSendChannel(
                        1, MPI_DOUBLE, my_rank - 1, 0, using_rt_send_buf,
                        using_sized_enqueue, PROCESS_CHANNEL_BUFFERED_MSG_SIZE,
                        PURE_COMM_WORLD);

                double neighbor_hi_val;
                // MPI_Recv(&neighbor_hi_val, 1, MPI_DOUBLE, my_rank - 1, 0,
                // MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                rc0 = pure_thread_global->GetOrInitRecvChannel(
                        1, MPI_DOUBLE, my_rank - 1, 0, false,
                        PROCESS_CHANNEL_BUFFERED_MSG_SIZE, PURE_COMM_WORLD);
                // rc0->Dequeue(&neighbor_hi_val);
                // rc0->Wait();

                pure_send_recv_user_bufs(sc0, &temp[0], 1, rc0,
                                         &neighbor_hi_val);
                sc0->Wait();
                rc0->Wait();

                a[0] = (neighbor_hi_val + temp[0] + temp[1]) / 3.0;

                // sc0->Wait();
            }

            // receive first elemetn of neighbor's array from rank above me
            if (my_rank < n_ranks - 1) {
                // send last elemenet of my array to the one above me
                // MPI_Send(&temp[n_per - 1], 1, MPI_DOUBLE, my_rank + 1, 0,
                // MPI_COMM_WORLD);
                const bool using_rt_send_buf   = false;
                const bool using_sized_enqueue = false;
                sc1 = pure_thread_global->GetOrInitSendChannel(
                        1, MPI_DOUBLE, my_rank + 1, 0, using_rt_send_buf,
                        using_sized_enqueue, PROCESS_CHANNEL_BUFFERED_MSG_SIZE,
                        PURE_COMM_WORLD);
                // sc1->EnqueueUserBuf(&temp[num_doubles - 1]);

                double neighbor_lo_val;
                // MPI_Recv(&neighbor_lo_val, 1, MPI_DOUBLE, my_rank + 1, 0,
                // MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                rc1 = pure_thread_global->GetOrInitRecvChannel(
                        1, MPI_DOUBLE, my_rank + 1, 0, false,
                        PROCESS_CHANNEL_BUFFERED_MSG_SIZE, PURE_COMM_WORLD);
                // rc1->Dequeue(&neighbor_lo_val);
                // rc1->Wait();

                pure_send_recv_user_bufs(sc1, &temp[num_doubles - 1], 1, rc1,
                                         &neighbor_lo_val);

                sc1->Wait();
                rc1->Wait();

                a[num_doubles - 1] = (temp[num_doubles - 2] +
                                      temp[num_doubles - 1] + neighbor_lo_val) /
                                     3.0;

                // sc1->Wait();
            }
        }
    }
    t.pause();

    free(a);
    free(temp);
}

int __orig_main(int argc, char* argv[], PureThread* const pure_thread) {

    const auto iter         = atoi(argv[1]);
    const auto cls_per_rank = atoi(argv[2]);

    int            rank;
    int            mpi_size;
    BenchmarkTimer timer("End-to-End");

    rank     = pure_thread->Get_rank();
    mpi_size = pure_thread->Get_size();
    PURE_BARRIER;

    bench(iter, cls_per_rank, rank, mpi_size, timer);

    if (rank == 0) {
        fprintf(stderr, "Ran with %d iter weight, %d ranks, %d array elts\n",
                iter_weight, mpi_size, cls_per_rank);
    }
    pure_thread_global->Finalize();
    PureRT::generate_stats(rank, timer);

    PURE_BARRIER;

    return EXIT_SUCCESS;
}