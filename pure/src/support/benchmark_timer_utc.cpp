// Author: James Psota
// File:   benchmark_timer_utc.cpp 

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
#include <sstream>

#include "pure/support/benchmark_timer.h"
#include "pure/support/helpers.h"

BenchmarkTimerUtc::BenchmarkTimerUtc(const std::string name_arg,
                                     bool process_and_thread_timers_arg)
    : name_(name_arg),
      process_and_thread_timers(process_and_thread_timers_arg) {

    start_time_monotonic_.tv_sec = start_time_monotonic_.tv_nsec = 0;
    start_time_process_cpu_.tv_sec = start_time_process_cpu_.tv_nsec = 0;
    start_time_thread_cpu_.tv_sec = start_time_thread_cpu_.tv_nsec = 0;

#if COLLECT_THREAD_TIMELINE_DETAIL
#if TIMER_INTERVAL_NUM_GUESS > 0
    timeline_intervals_.reserve(TIMER_INTERVAL_NUM_GUESS * 1.2);
#else
    timeline_intervals_.reserve(200);
#endif
#endif
}

BenchmarkTimerUtc::~BenchmarkTimerUtc() {}

// aggregate functions (when using straight MPI only)
uint64_t BenchmarkTimerUtc::all_ranks_min() {
    return all_ranks_allreduce_op(MPI_MIN);
}

uint64_t BenchmarkTimerUtc::all_ranks_max() {
    return all_ranks_allreduce_op(MPI_MAX);
}

double BenchmarkTimerUtc::all_ranks_avg() {
    uint64_t sum = all_ranks_allreduce_op(MPI_SUM);

    int sz;
    MPI_Comm_size(MPI_COMM_WORLD, &sz);

    return static_cast<double>(sum) / sz;
}

std::string BenchmarkTimerUtc::ToString() const {
    std::stringstream ss;
    ss << "Benchmark Timer " << name_ << ":"
       << " monotonic (wallclock) time:" << elapsed_ns_monotonic_ << "ns"
       << "\t"
       << " process CPU time: " << elapsed_ns_process_cpu_ << "ns"
       << "\t"
       << " process CPU utilization: "
       << percentage<uint64_t>(elapsed_ns_process_cpu_, elapsed_ns_monotonic_)
       << "%"
       << "\t"
       << " thread CPU time: " << elapsed_ns_thread_cpu_ << "ns"
       << "\t"
       << " thread CPU utilization: "
       << percentage<uint64_t>(elapsed_ns_thread_cpu_, elapsed_ns_monotonic_)
       << "%";
    return ss.str();
}

// static helper
void BenchmarkTimerUtc::print_portion(char const* label, uint64_t numerator,
                                      uint64_t denominator, FILE* outfile) {
    fprintf(outfile, "%s\t%" PRIu64 "\n", label, numerator);
    fprintf(outfile, "%s\t%.1lf%%\n", label,
            percentage<uint64_t>(numerator, denominator));
}

// private methods
uint64_t BenchmarkTimerUtc::all_ranks_allreduce_op(MPI_Op op) {
    assert_mpi_running();
    uint64_t elapsed = elapsed_ns_monotonic();
    uint64_t reduced_val;
    int      ret = MPI_Allreduce(&elapsed, &reduced_val, 1, MPI_UINT64_T, op,
                            MPI_COMM_WORLD);
    assert(ret == MPI_SUCCESS);
    return reduced_val;
}

//////////////////////////////////////////////////////////////
// Simplified C interface -- only a subset of functionality supported

void* construct_benchmark_timer(const char* name) {
    BenchmarkTimerUtc* bt = new BenchmarkTimerUtc(std::string(name));
    assert(bt != nullptr);
    return (reinterpret_cast<void*>(bt));
}

void deconstruct_benchmark_timer(void* bt) {
    assert(bt);
    delete (reinterpret_cast<BenchmarkTimerUtc*>(bt));
}

// static helper
BenchmarkTimerUtc* BenchmarkTimerUtc::get_benchmark_timer(void* bt) {
    return (reinterpret_cast<BenchmarkTimerUtc*>(bt));
}

void start_benchmark_timer(void* bt) {
    assert(bt);
    BenchmarkTimerUtc* bt_obj = reinterpret_cast<BenchmarkTimerUtc*>(bt);
    bt_obj->start();
}

void pause_benchmark_timer(void* bt) {
    assert(bt);
    BenchmarkTimerUtc* bt_obj = reinterpret_cast<BenchmarkTimerUtc*>(bt);
    bt_obj->pause();
}
