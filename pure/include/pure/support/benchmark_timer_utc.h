// Author: James Psota
// File:   benchmark_timer_utc.h 

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
#include "pure/support/colors.h"
#include "pure/support/helpers.h"

// if you increase this, change get_summarized_runtimes in benchmark_common.R to
// include the new custom fields (and of course use them in whatever script is
// analyzing your benchmarks.) Also change NUM_CUSTOM_TIMERS in
// $CPL/support/custom_timer_headers.rb
#define MAX_CUSTOM_TIMERS 18
//#define MAX_BT_STR_LEN (64 - 1)

using namespace PureRT;

#if COLLECT_THREAD_TIMELINE_DETAIL

struct TimerIntervalDetails {

    // clock monotonic for now. consider doing process and thread later
    // const std::string label; -- get directly from timer name later.
    // char            timer_name_[MAX_BT_STR_LEN + 1];
    struct timespec    start_clock_monotonic_, end_clock_monotonic_;
    BenchmarkTimerUtc* bt = nullptr;
    uint64_t           sequence_number_;
    int                partner_rank_;

    TimerIntervalDetails(const struct timespec& start_c_arg,
                         const struct timespec& end_c_arg, uint64_t seq_arg,
                         int partner_rank_arg)
        : start_clock_monotonic_(start_c_arg), end_clock_monotonic_(end_c_arg),
          sequence_number_(seq_arg), partner_rank_(partner_rank_arg) {

        // REMOVING NOTE
        // TODO: remove the note completely

        // assert(strlen(name_arg) <= MAX_STR_LEN);
        // assert(strlen(note_arg) <= MAX_STR_LEN);
        // strncpy(timer_name_, name_arg, MAX_STR_LEN);
        // strncpy(note_, note_arg, MAX_STR_LEN);
    }

    inline uint64_t NanosecDuration() const {
        return timespecDiff<true>(end_clock_monotonic_, start_clock_monotonic_);
    }
};

#endif

class BenchmarkTimerUtc {

  public:
    BenchmarkTimerUtc(const std::string /* name */,
                      bool process_and_thread_timers = false);
    virtual ~BenchmarkTimerUtc();

    inline void start() {
        int       ret;
        const int success_return_val = 0;

        ret = utc_time_monotonic(&start_time_monotonic_);
        assert(ret == success_return_val);
        if (process_and_thread_timers) {
            ret = utc_time_process_cpu(&start_time_process_cpu_);
            assert(ret == success_return_val);
            ret = utc_time_thread_cpu(&start_time_thread_cpu_);
            assert(ret == success_return_val);
        }
    }

    inline void pause(int comm_partner = -1) {

        int             ret;
        struct timespec now_ts;

        if (process_and_thread_timers) {
            // add the elapsed time so far since start_time to each relevant
            // timer
            ret = utc_time_thread_cpu(&now_ts);
            assert(ret == 0);
            elapsed_ns_thread_cpu_ +=
                    timespecDiff<false>(now_ts, start_time_thread_cpu_);
            ret = utc_time_process_cpu(&now_ts);
            assert(ret == 0);
            elapsed_ns_process_cpu_ +=
                    timespecDiff<false>(now_ts, start_time_process_cpu_);
        }

        // ***************************************************************
        // ***************************************************************
        // ***************************************************************
        // ***************************************************************
        // IMPORTANT!! This must go last as now_ts is used for the
        // TimerIntervalDetails
        ret = utc_time_monotonic(&now_ts);
        assert(ret == 0);
        elapsed_ns_monotonic_ +=
                timespecDiff<true>(now_ts, start_time_monotonic_);
        // ***************************************************************

#if COLLECT_THREAD_TIMELINE_DETAIL
        timeline_intervals_.emplace_back(start_time_monotonic_, now_ts,
                                         timer_seq_num++, comm_partner);
#endif
    }

    // Accessor functions

    // wallclock elapsed
    inline uint64_t elapsed_ns_monotonic() const {
        return elapsed_ns_monotonic_;
    }

    // process cpu time
    inline uint64_t elapsed_ns_process_cpu() const {
        return elapsed_ns_process_cpu_;
    }

    // thread cpu time
    inline uint64_t elapsed_ns_thread_cpu() const {
        return elapsed_ns_thread_cpu_;
    }

    // aggregate functions (when using straight MPI only)
    uint64_t all_ranks_min();
    uint64_t all_ranks_max();
    double   all_ranks_avg();

    inline std::string Name() const { return name_; }

    std::string ToString() const;

    inline void Print() const {
        printf(KBLU "%s\n" KRESET, ToString().c_str());
    }

#if COLLECT_THREAD_TIMELINE_DETAIL
    inline std::vector<TimerIntervalDetails>& IntervalDetails() {
        return timeline_intervals_;
    }

    inline struct timespec OriginTimespec() const {
        assert(timeline_intervals_.size() > 0);
        return timeline_intervals_[0].start_clock_monotonic_;
    }

    inline size_t NumIntervalDetails() const {
        return timeline_intervals_.size();
    }

#endif

    static BenchmarkTimerUtc* get_benchmark_timer(void* /* bt */);

    // static helper
    static void print_portion(char const* label, uint64_t numerator,
                              uint64_t denominator, FILE* outfile = stdout);

  private:
    std::string     name_;
    struct timespec start_time_monotonic_;
    struct timespec start_time_process_cpu_;
    struct timespec start_time_thread_cpu_;
    uint64_t        elapsed_ns_monotonic_   = 0;
    uint64_t        elapsed_ns_process_cpu_ = 0;
    uint64_t        elapsed_ns_thread_cpu_  = 0;
    uint64_t        timer_seq_num           = 1;
    bool            process_and_thread_timers;

    uint64_t all_ranks_allreduce_op(MPI_Op /*op*/);

#if COLLECT_THREAD_TIMELINE_DETAIL
    std::vector<TimerIntervalDetails> timeline_intervals_;
#endif

}; // class BenchmarkTimerUtc

/* clock type definitions on Linux:
      // N.B. http://lxr.free-electrons.com/source/include/uapi/linux/time.h#L48
      CLOCK_MONOTONIC: Represents monotonic time since some unspecified starting
   point. This clock cannot be set.
      CLOCK_PROCESS_CPUTIME_ID: High-resolution per-process timer from the CPU.
      CLOCK_THREAD_CPUTIME_ID: High-resolution per-thread timer from the CPU.
 */

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