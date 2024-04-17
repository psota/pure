// Author: James Psota
// File:   benchmark_timer.h 

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

#ifndef BENCHMARK_TIMER_H
#define BENCHMARK_TIMER_H

#include <assert.h>
#include <inttypes.h>

#ifdef __cplusplus

#pragma once

#include <cstring>
#include <string>
#include <vector>

#include "mpi.h"
#include "pure/3rd_party/low-overhead-timers/low_overhead_timers.h"
#include "pure/support/colors.h"
#include "pure/support/helpers.h"

// if you increase this, change get_summarized_runtimes in benchmark_common.R to
// include the new custom fields (and of course use them in whatever script is
// analyzing your benchmarks.)
// Also change NUM_CUSTOM_TIMERS in $CPL/support/misc/custom_timer_headers.rb
// and $CPL/support/misc/experiment_details.rb
// and possibly update thread timeline to fix up the utility calculations
#define MAX_CUSTOM_TIMERS 28

// should be done elsewhere
// using namespace PureRT;

#if COLLECT_THREAD_TIMELINE_DETAIL
// for arbirary passing of pure_thread variable name
#define PURE_START_TIMER(pt, timer) pt->timer.start()
#define PURE_PAUSE_TIMER(pt, timer, comm_partner, obj_ptr) \
    pt->timer.pause(comm_partner, obj_ptr)
#else
#define PURE_START_TIMER(pt, timer)
#define PURE_PAUSE_TIMER(pt, timer, comm_partner, obj_ptr)
#endif

#if PURE_APP_TIMER
#define START_TIMER(t) t.start();
#define PAUSE_TIMER(t) t.pause();
#else
#define START_TIMER(t)
#define PAUSE_TIMER(t)
#endif

#if COLLECT_THREAD_TIMELINE_DETAIL

class BenchmarkTimer;

struct TimerIntervalDetails {

    uint64_t        start_cycle_, end_cycle_;
    BenchmarkTimer* bt = nullptr;
    uint64_t        sequence_number_;
    int             partner_rank_;
    void*           object_ptr;

    TimerIntervalDetails(const uint64_t start_c_arg, const uint64_t end_c_arg,
                         uint64_t seq_arg, int partner_rank_arg,
                         void* object_ptr_arg = nullptr)
        : start_cycle_(start_c_arg), end_cycle_(end_c_arg),
          sequence_number_(seq_arg), partner_rank_(partner_rank_arg),
          object_ptr(object_ptr_arg) {}

    inline uint64_t CyclesDuration() const {
        assert(end_cycle_ > start_cycle_);
        return end_cycle_ - start_cycle_;
    }
};
#endif

class BenchmarkTimer {

  public:
    BenchmarkTimer(const std::string /* name */);
    virtual ~BenchmarkTimer();

    inline void start() { start_cycle_ = rdtscp(); }

    inline void pause(int comm_partner = -1, void* obj_ptr = nullptr) {
        uint64_t now_cycles = rdtscp();
#ifndef OSX
        check(start_cycle_ > 0,
              "%s timer: expected start_cycle_ > 0 but it is %ld",
              name_.c_str(), start_cycle_);
#endif
        // if (now_cycles < start_cycle_) {
        //     fprintf(stderr, "now %llu  start %llu\n", now_cycles,
        //     start_cycle_);
        // }
#ifndef OSX
        assert(now_cycles >= start_cycle_); // sometimes it can be equal at
                                            // least on osx
#endif
        elapsed_cycles_ += (now_cycles - start_cycle_);
#ifndef OSX
        assert(elapsed_cycles_ > 0);
        // ***************************************************************
#endif
        ++timer_seq_num_;
#if COLLECT_THREAD_TIMELINE_DETAIL
        timeline_intervals_.emplace_back(start_cycle_, now_cycles,
                                         timer_seq_num_, comm_partner, obj_ptr);
#endif
    }

    // only should be used in special situations where timing is done in, for
    // example, a C file and manually calculated but we still want to leverage
    // BenchmarkTimer functionality.
    inline void set_elapsed_cycles(uint64_t c) { elapsed_cycles_ = c; }

    // Accessor functions

    // wallclock elapsed
    inline uint64_t elapsed_cycles() const { return elapsed_cycles_; }

    inline uint64_t num_intervals() const { return timer_seq_num_; }

#if MPI_ENABLED
    // aggregate functions (when using straight MPI only)
    uint64_t all_ranks_min();
    uint64_t all_ranks_max();
    double   all_ranks_avg();
#endif

    inline std::string Name() const { return name_; }
    std::string        ToString() const;
    inline void Print() const { printf(KCYN "%s" KRESET, ToString().c_str()); }

#if COLLECT_THREAD_TIMELINE_DETAIL
    inline std::vector<TimerIntervalDetails>& IntervalDetails() {
        return timeline_intervals_;
    }

    inline uint64_t OriginCycles() const {
        // this is meant to be called on the end-to-end timer only
        assert(timeline_intervals_.size() > 0);
        return timeline_intervals_[0].start_cycle_;
    }

    inline uint64_t TerminationCycles() const {
        // this is meant to be called on the end-to-end timer only
        assert(timeline_intervals_.size() > 0);
        return timeline_intervals_[0].end_cycle_;
    }

    inline size_t NumIntervalDetails() const {
        return timeline_intervals_.size();
    }
#endif

    static BenchmarkTimer* get_benchmark_timer(void* /* bt */);

    // static helper
    static void print_portion(char const* label, uint64_t numerator,
                              uint64_t denominator, FILE* outfile = stdout);

  private:
    std::string name_;
    uint64_t    start_cycle_;
    uint64_t    elapsed_cycles_ = 0;
    uint64_t    timer_seq_num_  = 0;

#if MPI_ENABLED
    uint64_t all_ranks_allreduce_op(MPI_Op /*op*/);
#endif

#if COLLECT_THREAD_TIMELINE_DETAIL
    std::vector<TimerIntervalDetails> timeline_intervals_;
#endif

}; // class BenchmarkTimer

// ends __cplusplus
#endif

// C extensions -- only a subset of functionality supported

#ifdef __cplusplus
extern "C" {
#endif

void* construct_benchmark_timer(const char* /* name */);
void  deconstruct_benchmark_timer(void* /* bt */);
void  start_benchmark_timer(void* /* bt */);
void  pause_benchmark_timer(void* /* bt */);

#ifdef __cplusplus
};
#endif

#endif