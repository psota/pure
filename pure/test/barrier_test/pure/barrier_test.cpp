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

#include "pure.h"
#include "pure/support/benchmark_timer.h"
#include "pure/support/stats_generator.h"

using namespace std;
using namespace PureRT;

#define DO_SPLIT_TEST 0

void printTimer(BenchmarkTimer& timer, int rank) {
    printf("rank %d: ", rank);
    timer.Print();
}

int main(int argc, char* argv[]) {
    assert(argc >= 2);
    const int iter = atoi(argv[1]);

    MPI::Init(argc, argv);
    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    BenchmarkTimer pure_timer("Pure Barrier");
    BenchmarkTimer pure_timer2("Pure Barrier2");

    // #if DO_V1_BARRIER
    //     pure_timer.start();
    //     for (auto i = 0; i < iter; ++i) {
    //         pure_thread->BarrierOld(PURE_COMM_WORLD, true);
    //     }
    //     pure_timer.pause();
    // #endif
    pure_thread->Barrier(PURE_COMM_WORLD, true);

    pure_timer.start();
    for (auto i = 0; i < iter; ++i) {

        pure_thread->Barrier(PURE_COMM_WORLD, true);
    }
    pure_timer.pause();

#if DO_SPLIT_TEST
    PureComm* comm = pure_thread->PureCommSplit(PURE_RANK % 2, PURE_RANK);
    for (auto i = 0; i < iter; ++i) {
        pure_thread->Barrier(comm);
    }
    delete (comm);
#endif

    // MPI::Finalize();
    pure_thread->Finalize();

    // if (PURE_RANK == 0) {
    //     printf("Barrier time:  %llu\n", pure_timer.elapsed_cycles());
    //     printf("Barrier2 time: %llu\n", pure_timer2.elapsed_cycles());
    // }

    string stats = PureRT::generate_stats(rank, pure_timer);

    exit(0);
}