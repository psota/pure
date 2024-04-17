// Author: James Psota
// File:   allreduce_sum_one_double_iter.h 

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

#ifndef ALL_REDUCE_SUM_ONE_DOUBLE_PROCESS_H
#define ALL_REDUCE_SUM_ONE_DOUBLE_PROCESS_H

/* How to generalize to all applications:
  1. add support for more datatypes -- templatize
  2. use in ReduceChannel
  3. add support for arbitrary sizes via constructor
  4. point to buffer, not a dropbox apporach
  5. add support for more operations
  */

#include "pure/support/helpers.h"
#include <atomic>

using std::atomic;

using seq_type = std::uint_fast32_t;

struct alignas(CACHE_LINE_BYTES) SequencedReduceThreadData {
    alignas(CACHE_LINE_BYTES) atomic<seq_type> put_elt_seq;
    atomic<double> thread_elt;

    alignas(CACHE_LINE_BYTES) atomic<seq_type> put_reduced_seq;
    atomic<double> reduced_val;
    char _pad[CACHE_LINE_BYTES - sizeof(atomic<seq_type>) - sizeof(double)];

    SequencedReduceThreadData() { put_reduced_seq.store(-1); }
};

// There is one of these per Process, not thread!
// New Name: AllReduceSumOneDoubleProcessIter
class AllReduceSumOneDoubleProcessIter {

  public:
    AllReduceSumOneDoubleProcessIter()
        : wait_for_data("wait_for_data"), compute("compute"),
          store_flag("store_flag") {}
    ~AllReduceSumOneDoubleProcessIter() {
        free(thread_data);

        printf("reduce timers:\nwait:\t%lld\ncompute:\t%lld\nstore:\t%lld\n",
               wait_for_data.elapsed_cycles(), compute.elapsed_cycles(),
               store_flag.elapsed_cycles());
    }

    // every thread call this
    void Init(PureComm const* const pure_comm, int num_elts,
              seq_type& thread_seq) {

        assert(num_elts == 1); // only 1 supported
        if (pure_comm->GetThreadNumInCommInProcess() == 0) {
            const auto num_threads_in_pure_comm =
                    pure_comm->GetNumPureRanksThisProcess();
            thread_data = jp_memory_alloc<1, CACHE_LINE_BYTES, false,
                                          SequencedReduceThreadData>(
                    sizeof(SequencedReduceThreadData) *
                    num_threads_in_pure_comm);
            memset(thread_data, 0,
                   num_threads_in_pure_comm *
                           sizeof(SequencedReduceThreadData));
        }

        thread_seq = 1;
    }

    void Reduce(PureThread* const pure_thread, PureComm const* const pure_comm,
                int pure_comm_root, double const* const input_buf,
                double* const output_buf, seq_type& thread_seq) {

        const int thread_num_in_pure_comm =
                pure_comm->GetThreadNumInCommInProcess();
        const int pure_comm_size_this_process =
                pure_comm->GetNumPureRanksThisProcess();

        assert(thread_num_in_pure_comm < pure_comm_size_this_process);

        assert(pure_comm_root == 0); // hack for now
        const auto root_thread_num = 0;
        const auto is_root = (thread_num_in_pure_comm == root_thread_num);

        // STEP 1
        ++thread_seq;

        if (is_root) {

            // STEP:
            assert(thread_num_in_pure_comm == root_thread_num);
            double sum = input_buf[0]; // root's component

            // todo maybe OOOO
            assert(root_thread_num == 0);
            for (auto t = 1; t < pure_comm_size_this_process; ++t) {
                // wait_for_data.start();
                while (thread_data[t].put_elt_seq.load(
                               std::memory_order_acquire) != thread_seq) {
                    x86_pause_instruction();
                    MAYBE_WORK_STEAL(pure_thread);
                }
                // wait_for_data.pause();

                // compute.start();
                // it's been updated, add in the value
                sum += thread_data[t].thread_elt.load(
                        std::memory_order_acquire);
                // compute.pause();
            }

            // store_flag.start();
            // STEP : the threads can go ahead (potentially to the next use and
            // store their data)
            reduce_done_flag.store(thread_seq, std::memory_order_release);
            // store_flag.pause();

            // STEP: The reduction inside my process and pure comm is done,
            // but now do the intra-process reduction
#if MPI_ENABLED

            fprintf(stderr, "before mpi reduce\n");
            const auto ret = MPI_Reduce(&sum, output_buf, 1, MPI_DOUBLE,
                                        MPI_SUM, 0, pure_comm->GetMpiComm());
            fprintf(stderr, "  after mpi reduce\n");
            assert(ret == MPI_SUCCESS);
#else
            output_buf[0] = sum;
#endif
        } else {
            // not root

            // STEP 2: create and update my entry
            // can these be relaxed or something?
            thread_data[thread_num_in_pure_comm].thread_elt.store(
                    input_buf[0], std::memory_order_release);
            thread_data[thread_num_in_pure_comm].put_elt_seq.store(
                    thread_seq, std::memory_order_release);

            // STEP
            while (reduce_done_flag.load(std::memory_order_acquire) !=
                   thread_seq) {
                x86_pause_instruction();
            }
        }
    }

  private:
    alignas(CACHE_LINE_BYTES) SequencedReduceThreadData* thread_data;
    alignas(CACHE_LINE_BYTES) atomic<seq_type> reduce_done_flag = -1;

    // hack remove
    alignas(CACHE_LINE_BYTES) BenchmarkTimer wait_for_data, compute, store_flag;
};

#endif