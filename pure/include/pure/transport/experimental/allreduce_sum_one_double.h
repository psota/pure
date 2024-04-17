// Author: James Psota
// File:   allreduce_sum_one_double.h 

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

#include "pure/support/helpers.h"
#include <atomic>

using std::atomic;

using seq_type = std::uint_fast32_t;

struct alignas(CACHE_LINE_BYTES) SequencedReduceThreadData {
    alignas(CACHE_LINE_BYTES) atomic<seq_type> put_elt_seq;
    atomic<double> thread_elt;
    char           _pad[CACHE_LINE_BYTES - sizeof(atomic<seq_type>) -
              sizeof(atomic<double>)];

    SequencedReduceThreadData() {}
};

// There is one of these per Process, not thread!
// New Name: AllReduceSumOneDoubleProcess
class AllReduceSumOneDoubleProcess {

  public:
    AllReduceSumOneDoubleProcess() = default; // but you must call init
    ~AllReduceSumOneDoubleProcess() { free(thread_data); }

    void Init(PureComm const* const pure_comm, seq_type& thread_seq) {
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

    void AllReduce(PureThread* const     pure_thread,
                   PureComm const* const pure_comm,
                   double const* const input_buf, double* const output_buf,
                   seq_type& thread_seq) {

        const int thread_num_in_pure_comm =
                pure_comm->GetThreadNumInCommInProcess();
        const int pure_comm_size_this_process =
                pure_comm->GetNumPureRanksThisProcess();

        assert(thread_num_in_pure_comm < pure_comm_size_this_process);

        const auto root_thread_num = 0;
        const auto is_root = (thread_num_in_pure_comm == root_thread_num);

        // STEP 1
        ++thread_seq;

        if (is_root) {
            // STEP 3a: root loops through all the entries, creating the
            // sums and storing it in the output array
            assert(thread_num_in_pure_comm == root_thread_num);
            double sum = input_buf[0]; // root's component

// 0 seems to be faster for the summing (simpler, in-order)
#define ALLREDUCE_V3_ALLOW_OOO_SUMMING 0
#if ALLREDUCE_V3_ALLOW_OOO_SUMMING
            bool completed[pure_comm_size_this_process];
            std::memset(completed, 0,
                        pure_comm_size_this_process * sizeof(bool));

            int remaining = pure_comm_size_this_process - 1;
            completed[thread_num_in_pure_comm] = true;

            while (remaining > 0) {
                assert(root_thread_num == 0);
                for (auto t = 1; t < pure_comm_size_this_process; ++t) {
                    if (completed[t]) {
                        continue;
                    }

                    // otherwise, try to add this sum if it's been updated
                    // this takes 50% of the root time
                    const auto this_thread_seq =
                            thread_data[t].put_elt_seq.load(
                                    std::memory_order_acquire);

                    // this allows us to extract the integers from the
                    // 64-bit atomic value without using bit shifting (just
                    // memory accesses)

                    assert(this_thread_seq <= thread_seq);
                    if (this_thread_seq == thread_seq) {
                        // it's been updated, add in the value
                        sum += thread_data[t].thread_elt.load(
                                std::memory_order_acquire);

                        assert(completed[t] == false);
                        completed[t] = true;
                        --remaining;
#if DEBUG_PRINT_ALLREDUCE
                        fprintf(stderr,
                                "r%d  adding in element %d from thread %d. "
                                "% d "
                                " threads remaining to be added "
                                "in.output_buf[0] currently "
                                "%d\n",
                                thread_num_in_pure_comm,
                                thread_data[t].thread_elt.load(
                                        std::memory_order_acquire),
                                t, remaining, sum);
#endif
                    } else {
                        MAYBE_WORK_STEAL(pure_thread);
                    }
                }
            }
#endif

#if !ALLREDUCE_V3_ALLOW_OOO_SUMMING
            // this one seems to be faster
            assert(root_thread_num == 0);
            for (auto t = 1; t < pure_comm_size_this_process; ++t) {
                while (thread_data[t].put_elt_seq.load(
                               std::memory_order_acquire) != thread_seq) {
                    x86_pause_instruction();
                    MAYBE_WORK_STEAL(pure_thread);
                }

                // it's been updated, add in the value
                sum += thread_data[t].thread_elt.load(
                        std::memory_order_acquire);
            }
#endif

            // STEP 4: The reduction inside my process and pure comm is
            // done, but now do the intra-process reduction
            double gsum;
#if MPI_ENABLED
            MPI_Request req;
            const auto  ret = MPI_Allreduce(&sum, &gsum, 1, MPI_DOUBLE, MPI_SUM,
                                           pure_comm->GetMpiComm());
            assert(ret == MPI_SUCCESS);
            // MPI_Status status;
            // PureRT::do_mpi_wait_with_work_steal(pure_thread, &req, &status);
#else
            gsum = sum;
#endif

            // STEP 4: now the reduction is done; set all of the thread's
            // reduction flags (but myself)
            assert(root_thread_num == 0);
            reduced_val.store(gsum, std::memory_order_release);
            put_reduced_seq.store(thread_seq, std::memory_order_release);

            // update my val
            output_buf[0] = gsum;
        } else {
            // not root

            // STEP 2: create and update my entry
            // can these be relaxed or something?
            thread_data[thread_num_in_pure_comm].thread_elt.store(
                    input_buf[0], std::memory_order_release);
            thread_data[thread_num_in_pure_comm].put_elt_seq.store(
                    thread_seq, std::memory_order_release);

            // STEP 3b: wait until reduced value is available, and then take
            // my own copy
            while (put_reduced_seq.load(std::memory_order_acquire) !=
                   thread_seq) {
                x86_pause_instruction();
                MAYBE_WORK_STEAL(pure_thread);
            }

            // now the reduced value is there, so copy it and return
            output_buf[0] = reduced_val.load(std::memory_order_acquire);
        }

    } // AllReduce function

  private:
    alignas(CACHE_LINE_BYTES) SequencedReduceThreadData* thread_data;

    alignas(CACHE_LINE_BYTES) atomic<seq_type> put_reduced_seq = -1;
    atomic<double> reduced_val;
};

#endif