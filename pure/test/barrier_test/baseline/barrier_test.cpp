// Author: James Psota
// File:   barrier_test.cpp 

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

#if DO_OPENMP
#include "omp.h"
#else
#include "mpi.h"
#endif

#include "pure/support/benchmark_timer.h"
#include "pure/support/stats_generator.h"

using namespace std;

int main(int argc, char** argv) {

    int rank;

#if DO_OPENMP
    rank = omp_get_thread_num();
    fprintf(stderr, "%d num threads\n", omp_get_max_threads());
#else
    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
#endif

    assert(argc >= 2);
    int iter = atoi(argv[1]);

    BenchmarkTimer mpi_timer("MPI Barrier");

#if DO_OPENMP
#pragma omp barrier
#else
    MPI_Barrier(MPI_COMM_WORLD);
#endif

    mpi_timer.start();
#if DO_OPENMP
    // https://rookiehpc.github.io/openmp/docs/barrier/index.html
    omp_set_num_threads(omp_get_max_threads());
#pragma omp parallel
    {
        for (auto i = 0; i < iter; ++i) {
#pragma omp barrier
        }

    } // ends omp parallel

#else
    // mpi
    for (auto i = 0; i < iter; ++i) {
        MPI_Barrier(MPI_COMM_WORLD);
    }
#endif

    mpi_timer.pause();

#if DO_OPENMP == 0
    MPI_Finalize();
#endif
    string stats = PureRT::generate_stats(rank, mpi_timer);

    exit(0);
}
