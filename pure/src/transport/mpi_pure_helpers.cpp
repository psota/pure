// Author: James Psota
// File:   mpi_pure_helpers.cpp 

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


#include "pure/transport/mpi_pure_helpers.h"
#include "mpi.h"
#include "pure/runtime/pure_thread.h"

#include "pure.h"

#if DEBUG_CHECK
#include <mutex>
#include <unordered_set>
std::mutex              duplicate_comm_tag_mutex;
std::unordered_set<int> duplicate_comm_tag_checker_set;
#endif

namespace PureRT {

void do_mpi_wait_with_work_steal(PureThread* const pt, MPI_Request* const req,
                                 MPI_Status* status) {

    assert(pt == pure_thread_global);
#if PCV4_OPTION_EXECUTE_CONTINUATION_WORK_STEAL && \
        PCV4_OPTION_DIRECT_CHANNEL_WORK_STEAL
    while (1) {

        sentinel("Waiting on MPI using MPI_Test, which we know is likely slow. "
                 "Configure around this.");
        int        req_done;
        const auto ret_success = MPI_Test(req, &req_done, status);
        assert(ret_success == MPI_SUCCESS);
        if (req_done) {
            break;
        } else {
            /* basic algorithm:
                1. Try to work steal. Do some work if possible.
                2. If failed to work steal, then try again NUM_WS_TRIES times
                3. If succeeded to work steal, then try MPI_Test again
            */
            for (auto i = 0; i < PCV4_OPTION_DIRECT_CHANNEL_WORK_STEAL_TRIES;
                 ++i) {
                const auto ws_attempted = MAYBE_WORK_STEAL(pt);
                if (ws_attempted) {
                    // that would have presumably took some time, so try the
                    // test now. see if this helps or hurts.
                    break;
                }
                x86_pause_instruction();
            }

            // consider doing a nanosleep here
            // struct timespec ts = {0, 500000};
            // nanosleep(&ts, nullptr);

            // presumably we don't need to trigger the progress engine manually
            // as MPI_Test should do that

        } // ends else MPI_Test failed
    }     // while(1)
#else
    int wait_ret = MPI_Wait(req, status);
    assert(wait_ret == MPI_SUCCESS);
#endif
} // namespace PureRT

void do_mpi_wait_with_work_steal_orig(PureThread* const  pt,
                                      MPI_Request* const req,
                                      MPI_Status*        status) {

    assert(pt == pure_thread_global);
#if PCV4_OPTION_EXECUTE_CONTINUATION_WORK_STEAL && \
        PCV4_OPTION_DIRECT_CHANNEL_WORK_STEAL
    while (1) {
        int        req_done;
        const auto ret_success = MPI_Test(req, &req_done, status);
        assert(ret_success == MPI_SUCCESS);
        if (req_done) {
            break;
        } else {

            MAYBE_WORK_STEAL(pt);
        }
    } // while(1)
#else
    int wait_ret = MPI_Wait(req, status);
    assert(wait_ret == MPI_SUCCESS);
#endif
}

// PureComm functions
// must be called by all mpi ranks in the mpi_ranks vector
MPI_Comm create_mpi_comm_tag(const std::vector<int>& mpi_ranks,
                             MPI_Comm origin_comm, int unique_comm_tag) {

#if DEBUG_CHECK
    {
        std::lock_guard<std::mutex> lg(duplicate_comm_tag_mutex);
        const auto iter = duplicate_comm_tag_checker_set.find(unique_comm_tag);
        if (iter == duplicate_comm_tag_checker_set.end()) {
            duplicate_comm_tag_checker_set.insert(unique_comm_tag);
        } else {
            int rank = 0;
            MPI_Comm_rank(MPI_COMM_WORLD, &rank);
            sentinel("Duplicate comm tag, %d, which "
                     "was already used in "
                     "this MPI process (%d)",
                     unique_comm_tag, rank);
        }
    }
#endif

    assert(mpi_ranks.size() > 0);

    MPI_Group origin_group;
    MPI_Comm_group(origin_comm, &origin_group);

    // debugging
    int origin_comm_size, origin_comm_rank;
    int ret = MPI_Comm_size(origin_comm, &origin_comm_size);
    assert(ret == MPI_SUCCESS);
    ret = MPI_Comm_rank(origin_comm, &origin_comm_rank);
    assert(ret == MPI_SUCCESS);

    for (auto r : mpi_ranks) {
        check(r < origin_comm_size,
              "create_mpi_comm_tag was passed rank %d, but the origin comm has "
              "size %d",
              r, origin_comm_size);
    }

    MPI_Group new_group;
    MPI_Comm  new_comm;

    ret = MPI_Group_incl(origin_group, mpi_ranks.size(), mpi_ranks.data(),
                         &new_group);

    if (ret != MPI_SUCCESS) {
        fprintf(stderr,
                KYEL "[origin comm size is %d origin comm MPI rank is %d   "
                     "MPI_Group_incl "
                     "failure: size of ranks: %llu  origin_group is %d "
                     "origin comm is %d ret is %d (sleeping a bit...)\n" KRESET,
                origin_comm_size, origin_comm_rank, mpi_ranks.size(),
                origin_group, origin_comm, ret);
        for (auto r : mpi_ranks) {
            fprintf(stderr, " %d ", r);
        }
        fprintf(stderr, "\n");

        switch (ret) {
        case MPI_ERR_COMM:
            fprintf(stderr, "MPI_ERR_COMM\n");
            break;
        case MPI_ERR_ARG:
            fprintf(stderr, "MPI_ERR_ARG\n");
            break;
        case MPI_ERR_GROUP:
            fprintf(stderr, "MPI_ERR_GROUP\n");
            break;
        case MPI_ERR_INTERN:
            fprintf(stderr, "MPI_ERR_INTERN\n");
            break;
        case MPI_ERR_RANK:
            fprintf(stderr, "MPI_ERR_RANK\n");
            break;
        default:
            fprintf(stderr, "MPI_Group_incl unknown error %d\n", ret);
        }

        sleep(1);
    }

    assert(ret == MPI_SUCCESS);
    ret = MPI_Comm_create_group(origin_comm, new_group, unique_comm_tag,
                                &new_comm);
    assert(ret == MPI_SUCCESS);
    // MPI_Group_free(&origin_group);

    return new_comm;
}

// must be called by all mpi ranks in origin_comm
MPI_Comm create_mpi_comm(const std::vector<int>& mpi_ranks,
                         MPI_Comm                origin_comm) {

    MPI_Group origin_group;
    MPI_Comm_group(origin_comm, &origin_group);

    MPI_Group new_group;
    MPI_Comm  new_comm;
    int ret = MPI_Group_incl(origin_group, mpi_ranks.size(), mpi_ranks.data(),
                             &new_group);
    assert(ret == MPI_SUCCESS);
    ret = MPI_Comm_create(origin_comm, new_group, &new_comm);
    assert(ret == MPI_SUCCESS);
    // MPI_Group_free(&origin_group);

    return new_comm;
}

} // namespace PureRT
