// Author: James Psota
// File:   allreduce_sum_one_double_iter_v2.h 

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
  5. add support for more operations
  6. support for concurrent reductions

DONE
  3. add support for arbitrary sizes via constructor
  4. point to buffer, not a dropbox apporach

  */

#include "pure/support/helpers.h"
#include <atomic>

using std::atomic;

using seq_type = std::uint_fast32_t;

struct alignas(CACHE_LINE_BYTES) SequencedReduceThreadData {
    alignas(CACHE_LINE_BYTES) atomic<seq_type> put_elt_seq;
    atomic<double*> thread_elt;
    char _pad[CACHE_LINE_BYTES - sizeof(atomic<seq_type>) - sizeof(double*)];
};

// There is one of these per Process, not thread!
// New Name: AllReduceSumOneDoubleProcessIter
class AllReduceSumOneDoubleProcessIter {

  public:
    AllReduceSumOneDoubleProcessIter() = default; // but you must call init
    ~AllReduceSumOneDoubleProcessIter() {
        free(thread_data);
        free(process_reduce_buf_internal);
    }

    // every thread call this
    void Init(PureComm const* const pure_comm, size_t num_elts_arg,
              seq_type& thread_seq) {
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

            process_reduce_buf_internal =
                    jp_memory_alloc<1, CACHE_LINE_BYTES, false, double>(
                            num_elts_arg * sizeof(double));
        }

        num_elts   = num_elts_arg;
        thread_seq = 1;
    }

    void Reduce(PureThread* const pure_thread, PureComm const* const pure_comm,
                int pure_comm_root, double* const input_buf,
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

            double* process_reduce_buf;
#if MPI_ENABLED
            process_reduce_buf = process_reduce_buf_internal;
            assert_cacheline_aligned(process_reduce_buf_internal);
#else
            process_reduce_buf = output_buf;
#endif

            // root's component
            std::memcpy(process_reduce_buf, input_buf,
                        num_elts * sizeof(double));

            // todo maybe OOOO
            assert(root_thread_num == 0);
            for (auto t = 1; t < pure_comm_size_this_process; ++t) {
                while (thread_data[t].put_elt_seq.load(
                               std::memory_order_acquire) != thread_seq) {
                    x86_pause_instruction();
                    MAYBE_WORK_STEAL(pure_thread);
                }

                double* const thread_buf = thread_data[t].thread_elt.load(
                        std::memory_order_acquire);

                // it's been updated, add in the value
                // todo force vectorization
                for (auto i = 0; i < num_elts; ++i) {
                    process_reduce_buf[i] += thread_buf[i];
                }
            }

            // STEP : the threads can go ahead (potentially to the next use
            // and store their data)
            bcast_done_flag.store(thread_seq, std::memory_order_release);

            // STEP: The reduction inside my process and pure comm is done,
            // but now do the intra-process reduction
#if MPI_ENABLED
            const auto ret = MPI_Reduce(process_reduce_buf_internal, output_buf,
                                        num_elts, MPI_DOUBLE, MPI_SUM, 0,
                                        pure_comm->GetMpiComm());
            assert(ret == MPI_SUCCESS);
#endif
            // upon leaving, the root has the result in output_buf

        } else {
            // not root

            // STEP 2: create and update my entry
            // can these be relaxed or something?
            thread_data[thread_num_in_pure_comm].thread_elt.store(
                    input_buf, std::memory_order_relaxed);
            thread_data[thread_num_in_pure_comm].put_elt_seq.store(
                    thread_seq, std::memory_order_release);

            // STEP 3b: wait until reduced value is available, and then take
            // my own copy
            while (bcast_done_flag.load(std::memory_order_acquire) !=
                   thread_seq) {
                x86_pause_instruction();
            }
        }

    } // AllReduce function

  private:
    alignas(CACHE_LINE_BYTES) SequencedReduceThreadData* thread_data;
    alignas(CACHE_LINE_BYTES) atomic<seq_type> bcast_done_flag = -1;

    double* process_reduce_buf_internal;
    size_t  num_elts;
};

#endif