// Author: James Psota
// File:   benchmark_timer.cpp 

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


#include <limits>
#include <sstream>

#include "pure/support/benchmark_timer.h"
#include "pure/support/helpers.h"

BenchmarkTimer::BenchmarkTimer(const std::string name_arg) : name_(name_arg) {
#if COLLECT_THREAD_TIMELINE_DETAIL
    timeline_intervals_.reserve(TIMER_INTERVAL_NUM_GUESS);
#endif
}

BenchmarkTimer::~BenchmarkTimer() {
#if COLLECT_THREAD_TIMELINE_DETAIL
    if (timeline_intervals_.size() >= TIMER_INTERVAL_NUM_GUESS) {
        sentinel("timeline_intervals size at the end of the program is %d but "
                 "we only reserved %d. Increase Makefile variable "
                 "TIMER_INTERVAL_NUM_GUESS.",
                 timeline_intervals_.size(), TIMER_INTERVAL_NUM_GUESS);
    }
#endif
}

#if MPI_ENABLED
// aggregate functions (when using straight MPI only)
uint64_t BenchmarkTimer::all_ranks_min() {
    return all_ranks_allreduce_op(MPI_MIN);
}

uint64_t BenchmarkTimer::all_ranks_max() {
    return all_ranks_allreduce_op(MPI_MAX);
}

double BenchmarkTimer::all_ranks_avg() {
    uint64_t sum = all_ranks_allreduce_op(MPI_SUM);

    int sz;
    MPI_Comm_size(MPI_COMM_WORLD, &sz);

    return static_cast<double>(sum) / sz;
}

// private methods
uint64_t BenchmarkTimer::all_ranks_allreduce_op(MPI_Op op) {
    assert_mpi_running();
    uint64_t elapsed = elapsed_cycles();
    uint64_t reduced_val;
    int      ret = MPI_Allreduce(&elapsed, &reduced_val, 1, MPI_UINT64_T, op,
                            MPI_COMM_WORLD);
    assert(ret == MPI_SUCCESS);
    return reduced_val;
}

#endif

std::string BenchmarkTimer::ToString() const {
    std::stringstream ss;
    ss << "Benchmark Timer " << name_ << ":"
       << " cycles:\t" << elapsed_cycles() << std::endl;
    return ss.str();
}

// static helper
void BenchmarkTimer::print_portion(char const* label, uint64_t numerator,
                                   uint64_t denominator, FILE* outfile) {
    fprintf(outfile, "%s\t%" PRIu64 "\n", label, numerator);
    fprintf(outfile, "%s\t%.1lf%%\n", label,
            percentage<uint64_t>(numerator, denominator));
}

//////////////////////////////////////////////////////////////
// Simplified C interface -- only a subset of functionality supported

void* construct_benchmark_timer(const char* name) {
    BenchmarkTimer* bt = new BenchmarkTimer(std::string(name));
    assert(bt != nullptr);
    return (reinterpret_cast<void*>(bt));
}

void deconstruct_benchmark_timer(void* bt) {
    assert(bt);
    delete (reinterpret_cast<BenchmarkTimer*>(bt));
}

// static helper
BenchmarkTimer* BenchmarkTimer::get_benchmark_timer(void* bt) {
    return (reinterpret_cast<BenchmarkTimer*>(bt));
}

void start_benchmark_timer(void* bt) {
    assert(bt);
    BenchmarkTimer* bt_obj = reinterpret_cast<BenchmarkTimer*>(bt);
    bt_obj->start();
}

void pause_benchmark_timer(void* bt) {
    assert(bt);
    BenchmarkTimer* bt_obj = reinterpret_cast<BenchmarkTimer*>(bt);
    bt_obj->pause();
}
