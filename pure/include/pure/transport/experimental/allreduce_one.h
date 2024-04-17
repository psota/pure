// Author: James Psota
// File:   allreduce_one.h 

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

#define MAKE_SEQ(thread_seq, partial_sum) ((thread_seq << 32) | partial_sum)
#define GET_SEQ(ss) ((ss >> 32))
#define GET_ELT(ss) ((ss & 0xffffff))

struct alignas(CACHE_LINE_BYTES) SequencedSum {
    // [ seq | thread_elt]  --- use as integers
    atomic<uint64_t> seq_thread_elt;
    char             _pad[CACHE_LINE_BYTES - sizeof(atomic<uint64_t>)];
};

class AllReduceOne {

  public:
    AllReduceOne() = default;
    ~AllReduceOne() { free(thread_sums); }

    void Init(int num_threads) {
        thread_sums = jp_memory_alloc<1, CACHE_LINE_BYTES, false, SequencedSum>(
                sizeof(SequencedSum) * num_threads);
        memset(thread_sums, 0, num_threads * sizeof(SequencedSum));
    }

    void AllReduce(PureThread* const pure_thread, int rank_in_pure_comm,
                   int pure_comm_size, int const* const input_buf,
                   int* const output_buf, uint64_t& thread_seq) {

        assert(rank_in_pure_comm < pure_comm_size);

        // STEP 1: barrier threads to make sure threads don't overwrite value
        // until it's been consumed by all the other threads. may be able to get
        // around this somehow.
        pure_thread->Barrier(PURE_COMM_WORLD); // assume world comm

        // STEP 2: create and update my entry
        const uint64_t updated_sequenced_sum =
                MAKE_SEQ(thread_seq, input_buf[0]);
        thread_sums[rank_in_pure_comm].seq_thread_elt.store(
                updated_sequenced_sum);

        // STEP 3: loop through all the entries, creating the sums and storing
        // it in the output array
        output_buf[0] = 0;
        bool completed[pure_comm_size];
        memset(completed, 0, pure_comm_size * sizeof(bool));
        int remaining = pure_comm_size;

        while (remaining > 0) {
            for (auto t = 0; t < pure_comm_size; ++t) {
                if (completed[t]) {
                    continue;
                }

                // otherwise, try to add this sum
                const auto thread_ss = thread_sums[t].seq_thread_elt.load();
                assert(GET_SEQ(thread_ss) <= thread_seq);
                if (GET_SEQ(thread_ss) == thread_seq) {
                    // it's been updated, add in the value
                    output_buf[0] += GET_ELT(thread_ss);
                    assert(completed[t] == false);
                    completed[t] = true;
                    --remaining;
                    // fprintf(stderr,
                    //         "r%d  adding in element %d from thread %d. %d "
                    //         "threads "
                    //         "remaining to be added in. output_buf[0]
                    //         currently "
                    //         "%d\n",
                    //         rank_in_pure_comm, GET_ELT(thread_ss), t,
                    //         remaining, output_buf[0]);
                }
            }
        }

        // STEP
        ++thread_seq;
    }

  private:
    SequencedSum* thread_sums;
};

#endif