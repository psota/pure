// Author: James Psota
// File:   reduce_one.h 

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

#ifndef REDUCE_ONE_H
#define REDUCE_ONE_H

#include "pure/support/helpers.h"
#include <atomic>

using std::atomic;

class ReduceOne {

  public:
    ReduceOne() = default;

    void Reduce(int rank_in_pure_comm, int pure_comm_size,
                int const* const my_buf, int* const root_buf_out,
                bool& thread_phase) {

        // wait until previous one done
        while (process_phase.load(std::memory_order_acquire) != thread_phase)
            ;

        // atomically add in mine
        // try relaxed here
        // sum.fetch_add(my_buf[0], std::memory_order_release);
        sum.fetch_add(my_buf[0], std::memory_order_relaxed);

        // reset sum, etc.
        if (rank_in_pure_comm == root_pure_rank_in_comm) {
            // wait until everyone (but me) is done
            while (num_completed.load(std::memory_order_acquire) <
                   pure_comm_size - 1)
                ;

            num_completed.store(0); // reset
            root_buf_out[0] = sum.load();
            sum.store(0); // reset
            process_phase.store(!process_phase, std::memory_order_release);
        } else {
            // increment num_completed if not root, incidating that I'm done
            num_completed.fetch_add(1, std::memory_order_relaxed);
            // ++num_completed;
        }
    }

    static const bool starting_thread_phase = true;

  private:
    const int root_pure_rank_in_comm = 0;

    alignas(CACHE_LINE_BYTES) atomic<unsigned int> num_completed = 0;

    alignas(CACHE_LINE_BYTES) atomic<int> sum = 0.0f;

    // thread phase should start as true
    alignas(CACHE_LINE_BYTES) atomic<bool> process_phase = true;
    char _pad[CACHE_LINE_BYTES - sizeof(atomic<int>)];
};

#endif