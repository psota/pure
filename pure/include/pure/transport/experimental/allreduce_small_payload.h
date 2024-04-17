// Author: James Psota
// File:   allreduce_small_payload.h 

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

#include "mpi.h"
#include "pure/common/pure_rt_enums.h"
#include "pure/runtime/pure_comm.h"
#include "pure/runtime/pure_thread.h"
#include "pure/support/helpers.h"
#include <atomic>

using std::atomic;

template <typename T, unsigned int count>
struct alignas(CACHE_LINE_BYTES) SequencedReduceThreadData {
    alignas(CACHE_LINE_BYTES) atomic<seq_type> put_elt_seq;
    T thread_elts[count];

    SequencedReduceThreadData() {}

    static_assert(sizeof(T) <= 8, "type must be a double or smaller");
};

// There is one of these per Process, not thread!
template <typename T, unsigned int max_count = MAX_COUNT_SMALL_PAYLOAD_AR_CHAN>
class AllReduceSmallPayloadProcess {

  public:
    AllReduceSmallPayloadProcess() = default; // but you must call init
    ~AllReduceSmallPayloadProcess() {
        free(thread_data);
        free(final_reduced_elts);
    }

    // called by each thread
    void Init(PureComm* const pure_comm, unsigned int count,
              seq_type& thread_seq) {

        if (pure_comm->GetThreadNumInCommInProcess() == 0) {
            const auto num_threads_in_pure_comm =
                    pure_comm->GetNumPureRanksThisProcess();
            thread_data =
                    jp_memory_alloc<1, CACHE_LINE_BYTES, false,
                                    SequencedReduceThreadData<T, max_count>>(
                            sizeof(SequencedReduceThreadData<T, max_count>) *
                            num_threads_in_pure_comm);
            memset(thread_data, 0,
                   num_threads_in_pure_comm *
                           sizeof(SequencedReduceThreadData<T, max_count>));

            final_reduced_elts = jp_memory_alloc<1, CACHE_LINE_BYTES, false, T>(
                    count * sizeof(T));
        }

        thread_seq = 1;

        // make sure the buf init is done before anyone uses this. should be
        // done outside main driver loop of any application.
        pure_comm->Barrier();
    }

    void AllReduce(PureThread* const     pure_thread,
                   PureComm const* const pure_comm, T const* const input_buf,
                   T* const output_buf, unsigned int count_arg,
                   MPI_Datatype datatype, MPI_Op op, seq_type& thread_seq) {

        // todo verify that mpi datatype aligns conceptually with T
        assert(datatype == MPI_INT || datatype == MPI_DOUBLE ||
               datatype == MPI_LONG_LONG_INT);
        assert(op == MPI_SUM || op == MPI_MIN || op == MPI_MAX);
        assert(count_arg > 0);
        check_always(count_arg <= max_count,
                     "count should be less than or equal to %d but is %d. "
                     "MAX_COUNT_SMALL_PAYLOAD_AR_CHAN is %d",
                     max_count, count_arg, MAX_COUNT_SMALL_PAYLOAD_AR_CHAN);

        const auto count       = count_arg;
        const auto count_bytes = count * sizeof(T);

        const int thread_num_in_pure_comm =
                pure_comm->GetThreadNumInCommInProcess();
        const int pure_comm_size_this_process =
                pure_comm->GetNumPureRanksThisProcess();
        const auto root_thread_num = 0;
        const auto is_root = (thread_num_in_pure_comm == root_thread_num);

        assert(thread_num_in_pure_comm < pure_comm_size_this_process);

        // STEP 1
        ++thread_seq;

        if (is_root) {
            // STEP 3a: root loops through all the entries, creating the
            // sums and storing it in the output array
            assert(thread_num_in_pure_comm == root_thread_num);
            T local_reduction[count];
            std::memcpy(local_reduction, input_buf, count_bytes);

// 0 seems to be faster for the summing (simpler, in-order)
#define ALLREDUCE_V3_ALLOW_OOO_SUMMING 0
#if ALLREDUCE_V3_ALLOW_OOO_SUMMING
            sentinel("this code path broken");
#else
            // this one seems to be faster
            assert(root_thread_num == 0);
            for (auto t = 1; t < pure_comm_size_this_process; ++t) {
                while (thread_data[t].put_elt_seq.load(
                               std::memory_order_acquire) != thread_seq) {
                    x86_pause_instruction();
                    MAYBE_WORK_STEAL(pure_thread);
                }

                // is this ok to not be atomic?
                if (op == MPI_SUM) {
                    for (auto i = 0; i < count; ++i) {
                        local_reduction[i] += thread_data[t].thread_elts[i];
                    }
                } else if (op == MPI_MIN) {
                    for (auto i = 0; i < count; ++i) {
                        if (thread_data[t].thread_elts[i] <
                            local_reduction[i]) {
                            // update min
                            local_reduction[i] = thread_data[t].thread_elts[i];
                        }
                    }
                } else if (op == MPI_MAX) {
                    for (auto i = 0; i < count; ++i) {
                        if (thread_data[t].thread_elts[i] >
                            local_reduction[i]) {
                            // update max
                            local_reduction[i] = thread_data[t].thread_elts[i];
                        }
                    }
                }
            }
#endif

            // STEP 4: The reduction inside my process and pure comm is
            // done, but now do the intra-process reduction
#if MPI_ENABLED
            MPI_Request req;
            const auto  ret =
                    MPI_Allreduce(local_reduction, final_reduced_elts, count,
                                  datatype, op, pure_comm->GetMpiComm());
            assert(ret == MPI_SUCCESS);
#else
            std::memcpy(final_reduced_elts, local_reduction, count_bytes);
#endif

            // STEP 4: now the reduction is done; set all of the thread's
            // reduction flags (but myself)
            assert(root_thread_num == 0);
            put_reduced_seq.store(thread_seq, std::memory_order_release);

            // update my thread-local val
            std::memcpy(output_buf, final_reduced_elts, count_bytes);
        } else {
            // not root

            // STEP 2: create and update my entry
            // can these be relaxed or something?
            std::memcpy(thread_data[thread_num_in_pure_comm].thread_elts,
                        input_buf, count_bytes);

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
            std::memcpy(output_buf, final_reduced_elts, count_bytes);
        }

    } // AllReduce function

  private:
    alignas(CACHE_LINE_BYTES)
            SequencedReduceThreadData<T, max_count>* thread_data;

    alignas(CACHE_LINE_BYTES) atomic<seq_type> put_reduced_seq = -1;
    T* final_reduced_elts;
};

#endif