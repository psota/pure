// Author: James Psota
// File:   hybrid_barrier.h 

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

#ifndef HYBRID_BARRIER_H
#define HYBRID_BARRIER_H

#define IN_ORDER_BARRIER_CHECK 1

#include "pure/support/helpers.h"
#include "pure/transport/mpi_pure_helpers.h"
#include <atomic>

using std::atomic;
using seq_type = std::uint_fast32_t;

class PureThread;

struct alignas(CACHE_LINE_BYTES) SequencedBarrierThreadData {
    alignas(CACHE_LINE_BYTES) atomic<seq_type> non_leader_arrived_seq;
};

class HybridBarrier {

  public:
    HybridBarrier(int max_num_threads) {
        const auto sz = sizeof(SequencedBarrierThreadData) * max_num_threads;
        thread_data   = jp_memory_alloc<1, CACHE_LINE_BYTES, false,
                                      SequencedBarrierThreadData>(sz);
        memset(thread_data, 0, sz);
    }

    ~HybridBarrier() { free(thread_data); }

    void Barrier(PureThread* const pure_thread, int thread_num_in_pure_comm,
                 int num_threads_this_process_this_comm, seq_type& thread_seq,
                 int root_thread_num, MPI_Comm mpi_comm,
                 bool do_work_stealing_while_waiting) {

        assert(thread_num_in_pure_comm < num_threads_this_process_this_comm);
        const auto is_leader = (thread_num_in_pure_comm == root_thread_num);

        // STEP 1
        ++thread_seq;

        if (is_leader) {
#if MPI_ENABLED
            // step 1: thread 0 of this process does an MPI_Barrier, only if
            // running with multiple processes
            auto const ret = MPI_Barrier(mpi_comm);
            assert(ret == MPI_SUCCESS);
#endif

            // STEP 3a: root loops through all the entries, creating the sums
            // and storing it in the output array
            assert(thread_num_in_pure_comm == root_thread_num);
            assert(root_thread_num == 0);
            assert(num_threads_this_process_this_comm > 0);

            // try different in order version! // constant defined above
#if IN_ORDER_BARRIER_CHECK
            for (auto t = 1; t < num_threads_this_process_this_comm; ++t) {
                while (thread_data[t].non_leader_arrived_seq.load(
                               std::memory_order_acquire) != thread_seq)
                    ;
            }
#else
            bool done[num_threads_this_process_this_comm];
            std::memset(done, 0,
                        sizeof(bool) * num_threads_this_process_this_comm);
            unsigned int num_done = 1; /// me

            while (num_done < num_threads_this_process_this_comm) {
                for (auto t = 1; t < num_threads_this_process_this_comm; ++t) {
                    if (done[t]) {
                        continue;
                    } else {
                        if (thread_data[t].non_leader_arrived_seq.load(
                                    std::memory_order_acquire) == thread_seq) {
                            done[t] = true;
                            ++num_done;
                        }
                    }
                }
            }
#endif

            // STEP 5: barrier done; set all of the thread's
            // barrier flags (but myself)
            barrier_done_seq.store(thread_seq, std::memory_order_release);
        } else {
            // STEP 2: create and update my entry
            thread_data[thread_num_in_pure_comm].non_leader_arrived_seq.store(
                    thread_seq, std::memory_order_release);

            // wait til done
            while (barrier_done_seq.load(std::memory_order_acquire) !=
                   thread_seq) {
                x86_pause_instruction();
            }
        }
    }

  private:
    alignas(CACHE_LINE_BYTES) SequencedBarrierThreadData* thread_data;
    alignas(CACHE_LINE_BYTES) atomic<seq_type> barrier_done_seq;
};

#endif