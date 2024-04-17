// Author: James Psota
// File:   pending_channel_manager.h 

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

#ifndef PENDING_CHANNEL_MANAGER_H
#define PENDING_CHANNEL_MANAGER_H

#include <vector>
#include <algorithm>

// TODO: optimization: reuse positions in vector. right now, just delete
// from inside avail and not avail slots
//  [ a | a | n | a | n | n ]

template <class ChanType>
class PendingChannelManager {

  private:
    std::vector<ChanType*> pending_chans;

  public:
    inline int AddPendingQueue(ChanType* pc) {
#if DEBUG_CHECK
        // make sure pc isn't already on the pending dequeues list
        auto it = find(pending_chans.begin(), pending_chans.end(), pc);
        if (it != pending_chans.end()) {
            fprintf(stderr, "Trying to add pc %p to pending_chans but "
                            "it's already there.\nCurrent pending channels:\n",
                    pc);
            for (auto const& pc : pending_chans) {
                fprintf(stderr, "-  %p\n", pc);
            }
            sentinel("Exiting.");
        }
#endif

        pending_chans.push_back(pc);
        // so PC can keep track of where it was inserted -- for use with
        // RemoveMatchingDequeueCandidate
        return pending_chans.size() - 1;
    }

    inline size_t NumPendingQueues() const {
        size_t sz = pending_chans.size();
        assert(sz >= 0);
        return sz;
    }

    inline ChanType* PendingQueueCandidate(int idx) const {
        // always start with the most recently inserted one, as deletions will
        // be faster (I think). The caller will likely try multiple candidates
        // to find one
        // caller must never call this more than NumPendingDequeues times
        // (otherwise we will go off the end of the array)
        // attempt starts with zero
        assert(idx >= 0);
        return pending_chans[idx];
    }

    inline void RemovePendingQueueCandidate(int idx_to_remove) {
        assert(idx_to_remove >= 0);
        pending_chans.erase(pending_chans.begin() + idx_to_remove);
    }

    inline void RemoveMatchingQueueCandidate(ChanType const* const pc_to_remove,
                                             int max_index_guess) {

        auto curr_max_index = pending_chans.size() - 1;
        auto start_idx      = curr_max_index > max_index_guess ? max_index_guess
                                                          : curr_max_index;
        assert(start_idx >= 0);
        assert(start_idx < pending_chans.size());

        for (auto i = start_idx; i >= 0; --i) {
            if (pending_chans[i] == pc_to_remove) {
                pending_chans.erase(pending_chans.begin() + i);
                return;
            }
        }

        // should never get here
        sentinel("RemoveMatchingDequeueCandidate: channel at %p was "
                 "not found in pending_chans for PureThread "
                 "after starting with max_index_guess = %d and "
                 "counting "
                 "down to zero.",
                 pc_to_remove, max_index_guess);
    }

    inline void EmptyPendingQueueCandidates() {
        assert(pending_chans.size() == 1);
        pending_chans.clear();
        assert(pending_chans.size() == 0);
    }

    inline void VerifyNoMembership(ChanType* c) {
#if DEBUG_CHECK
        auto pending_chans_it =
                std::find(pending_chans.begin(), pending_chans.end(), c);
        if (pending_chans_it == pending_chans.end())
            return;
        else
            sentinel("Unexpectedly found channel %p in pending_channels", c);
#endif
    }
};

#endif