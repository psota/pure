// Author: James Psota
// File:   allreduce_one_v2.h 

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

#ifndef ALLREDUCE_ONE_H
#define ALLREDUCE_ONE_H

#include "pure/support/helpers.h"
#include <atomic>

using std::atomic;

#define MAKE_SEQUENCED_VAL(thread_seq, partial_sum) \
    ((thread_seq << 32) | partial_sum)
#define GET_SEQ(ss) ((ss >> 32))
#define GET_VAL(ss) ((ss & 0xffffff))

struct alignas(CACHE_LINE_BYTES) SequencedSum {
    // [ seq | thread_elt]  --- use as integers
    alignas(CACHE_LINE_BYTES) atomic<uint64_t> seq_thread_elt;

    //   0             1
    // [ reduced_val | seq  ] --- can treat this as an int array
    alignas(CACHE_LINE_BYTES) atomic<uint64_t> seq_reduced_val = -1;
    char _pad[CACHE_LINE_BYTES - sizeof(atomic<uint64_t>)];
};

class AllReduceOneV2 {

  public:
    AllReduceOneV2() = default;
    ~AllReduceOneV2() { free(thread_sums); }

    void Init(int num_threads) {
        thread_sums = jp_memory_alloc<1, CACHE_LINE_BYTES, false, SequencedSum>(
                sizeof(SequencedSum) * num_threads);
        memset(thread_sums, 0, num_threads * sizeof(SequencedSum));
    }

    void AllReduce(PureThread* const pure_thread, int rank_in_pure_comm,
                   int pure_comm_size, int const* const input_buf,
                   int* const output_buf, uint64_t& thread_seq) {

        assert(rank_in_pure_comm < pure_comm_size);

        const auto root_rank = 0;
        const auto is_root   = (rank_in_pure_comm == root_rank);

        // STEP 1
        ++thread_seq;

        // STEP 2: create and update my entry
        const uint64_t updated_sequenced_sum =
                MAKE_SEQUENCED_VAL(thread_seq, input_buf[0]);
        thread_sums[rank_in_pure_comm].seq_thread_elt.store(
                updated_sequenced_sum, std::memory_order_release);

        if (is_root) {
            // STEP 3a: root loops through all the entries, creating the sums
            // and storing it in the output array
            assert(rank_in_pure_comm == root_rank);
            int sum = input_buf[0]; // root's component

            bool completed[pure_comm_size];
            std::memset(completed, 0, pure_comm_size * sizeof(bool));

            int remaining                = pure_comm_size - 1;
            completed[rank_in_pure_comm] = true;

            while (remaining > 0) {
                assert(root_rank == 0);
                for (auto t = 1; t < pure_comm_size; ++t) {
                    if (completed[t]) {
                        continue;
                    }

                    // otherwise, try to add this sum if it's been updated
                    // this takes 50% of the root time
                    uint64_t thread_ss = thread_sums[t].seq_thread_elt.load(
                            std::memory_order_acquire);

                    // this allows us to extract the integers from the 64-bit
                    // atomic value without using bit shifting (just memory
                    // accesses)
                    int* thread_ss_ints = reinterpret_cast<int*>(&thread_ss);
                    const auto this_thread_seq = thread_ss_ints[1];

                    assert(this_thread_seq <= thread_seq);
                    if (this_thread_seq == thread_seq) {
                        // it's been updated, add in the value
                        sum += thread_ss_ints[0];

                        assert(completed[t] == false);
                        completed[t] = true;
                        --remaining;
#if DEBUG_PRINT_ALLREDUCE
                        fprintf(stderr,
                                "r%d  adding in element %d from thread %d. % d "
                                " threads remaining to be added "
                                "in.output_buf[0] currently "
                                "%d\n",
                                rank_in_pure_comm, GET_VAL(thread_ss), t,
                                remaining, sum);
#endif
                    }
                }
            }

            // STEP 4: now the reduction is done; set all of the thread's
            // reduction flags (but myself)
            assert(root_rank == 0);
            const auto unified_val = MAKE_SEQUENCED_VAL(thread_seq, sum);
            for (auto t = 1; t < pure_comm_size; ++t) {
                thread_sums[t].seq_reduced_val.store(unified_val,
                                                     std::memory_order_release);
            }

            // update my val
            output_buf[0] = sum;
        } else {
            // not root
            // STEP 3b: wait until reduced value is available, and then take my
            // own copy
            uint64_t this_seq_reduced_val;
            while (1) {
                this_seq_reduced_val =
                        thread_sums[rank_in_pure_comm].seq_reduced_val.load(
                                std::memory_order_acquire);
                if (GET_SEQ(this_seq_reduced_val) == thread_seq) {
                    break;
                }
            }

            // now the reduced value is there, so copy it and return
            output_buf[0] = GET_VAL(this_seq_reduced_val);
        }
    }

  private:
    alignas(CACHE_LINE_BYTES) SequencedSum* thread_sums;
};

#endif