// Author: James Psota
// File:   batcher_manager.h 

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

#ifndef BATCHER_MANAGER_H
#define BATCHER_MANAGER_H

#include "pure/transport/experimental/direct_channel_batcher.h"
#include <atomic>

using namespace PureRT;

class BatcherManager {

  public:
    BatcherManager(int num_batchers_arg, int mate_mpi_rank,
                   EndpointType endpoint_type, int threads_per_process)
        : num_batchers(num_batchers_arg), curr_idx(0) {

        batchers = new DirectChannelBatcher*[num_batchers];
        for (auto b = 0; b < num_batchers; ++b) {
            const auto is_first_batcher = (b == 0);
            batchers[b] = new DirectChannelBatcher(mate_mpi_rank, endpoint_type,
                                                   threads_per_process,
                                                   is_first_batcher);
        }

        fprintf(stderr, "DirectChannelBatcher: %d bytes send threshold\n",
                send_bytes_threshold);
    }

    ~BatcherManager() {
        for (auto b = 0; b < num_batchers; ++b) {
            delete (batchers[b]);
        }
        delete[](batchers);
    }

    void Finalize() {
        for (auto b = 0; b < num_batchers; ++b) {
            batchers[b]->Finalize();
        }
    }

    DirectChannelBatcher* GetCurrBatcher() const {
        return batchers[curr_idx.load()];
    }

    // doesn't actually change it
    DirectChannelBatcher* GetNextBatcher() const { return batchers[NextIdx()]; }

    int IncrementBatcher() {
        auto const next = NextIdx();
        curr_idx.store(next);
        return next;
    }

  private:
    DirectChannelBatcher** batchers;
    int                    num_batchers;
    alignas(CACHE_LINE_BYTES) atomic<int> curr_idx;

    //////////////
    int NextIdx() const {
        const auto curr = curr_idx.load();
        if (curr == num_batchers - 1) {
            return 0;
        } else {
            return curr + 1;
        }
    }
};

#endif