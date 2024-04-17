// Author: James Psota
// File:   rand_jacobi.cpp 

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

#include "pure/support/benchmark_timer.h"
#include "pure/support/stats_generator.h"

#include "mpi.h"

using namespace std;

const int iter_weight = ITER_WEIGHT;

double f(double input, int rank) {
    double s = 0.0f;
    for (auto i = 0; i < iter_weight * rank; ++i) {
        s += i;
    }
    return s;
}

void bench(int iter, int cls_per_rank, int my_rank, int n_ranks,
           BenchmarkTimer& t) {

    int num_doubles = CACHE_LINE_BYTES / sizeof(double) * cls_per_rank;
    assert(num_doubles > 0);
    double a[num_doubles], temp[num_doubles];

    // fill a with something deterministic
    for (auto i = 0; i < num_doubles; ++i) {
        a[i] = i;
    }

    for (auto it = 0; it < iter; ++it) {

        if (it == 1) {
            MPI_Barrier(MPI_COMM_WORLD);
            t.start();
        }

        for (auto i = 0; i < num_doubles; ++i) {
            // compute step
            temp[i] = f(a[i], my_rank);
        }

        // compute inner elements of a from temp
        for (auto i = 1; i < num_doubles - 1; ++i) {
            a[i] = (temp[i - 1] + temp[i] + temp[i + 1]) / 3.0;
        }

        if (n_ranks > 1) {
            if (my_rank > 0) {
                // send first element of my array to rank below me
                MPI_Send(&temp[0], 1, MPI_DOUBLE, my_rank - 1, 0,
                         MPI_COMM_WORLD);
                double neighbor_hi_val;
                MPI_Recv(&neighbor_hi_val, 1, MPI_DOUBLE, my_rank - 1, 0,
                         MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                a[0] = (neighbor_hi_val + temp[0] + temp[1]) / 3.0;
            }

            // receive first elemetn of neighbor's array from rank above me
            if (my_rank < n_ranks - 1) {
                // send last elemenet of my array to the one above me
                MPI_Send(&temp[num_doubles - 1], 1, MPI_DOUBLE, my_rank + 1, 0,
                         MPI_COMM_WORLD);
                double neighbor_lo_val;
                MPI_Recv(&neighbor_lo_val, 1, MPI_DOUBLE, my_rank + 1, 0,
                         MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                a[num_doubles - 1] = (temp[num_doubles - 2] +
                                      temp[num_doubles - 1] + neighbor_lo_val) /
                                     3.0;
            }
        }
    }
    t.pause();
}

int main(int argc, char* argv[]) {

    const auto iter       = atoi(argv[1]);
    const auto array_elts = atoi(argv[2]);

    MPI::Init(argc, argv);
    int            rank;
    int            mpi_size;
    BenchmarkTimer timer("End-to-End");

    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &mpi_size);
    MPI_Barrier(MPI_COMM_WORLD);

    bench(iter, array_elts, rank, mpi_size, timer);

    if (rank == 0) {
        fprintf(stderr, "Ran with %d iter weight, %d ranks, %d array elts\n",
                iter_weight, mpi_size, array_elts);
    }

    string stats = PureRT::generate_stats(rank, timer);
    MPI_Barrier(MPI_COMM_WORLD);
    MPI::Finalize();

    exit(EXIT_SUCCESS);
}