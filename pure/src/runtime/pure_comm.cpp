// Author: James Psota
// File:   pure_comm.cpp 

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

#include "pure/runtime/pure_comm.h"
#include "mpi.h"
#include "pure/common/experimental/hybrid_barrier.h"
#include "pure/runtime/mpi_comm_split_manager.h"
#include "pure/runtime/pure_process.h"
#include "pure/runtime/pure_thread.h"
#include "pure/support/colors.h"
#include "pure/support/helpers.h"
#include "pure/support/zed_debug.h"
#include "pure/transport/mpi_pure_helpers.h"
#include <atomic>
#include <set>
#include <vector>

// initialize
atomic<int> PureCommProcessDetail::internal_id_process_cnt = 0;

PureCommProcessDetail::PureCommProcessDetail(
        PureCommProcessDetail* origin_process_detail_arg,
        int                    max_threads_per_process)
    : origin_process_detail(origin_process_detail_arg),
      internal_id(internal_id_process_cnt++),
      hybrid_barrier(max_threads_per_process) {
    split_details = new PureCommSplitDetails[max_threads_per_process];
}

PureCommProcessDetail::~PureCommProcessDetail() {
    if (mpi_comm != 0) {
        // auto ret = MPI_Comm_free(&mpi_comm);
        // assert(ret == MPI_SUCCESS);
    }
    delete[](split_details);
}

void PureCommProcessDetail::SanityCheckMpiRank(int rank) const {
#if DEBUG_CHECK && MPI_ENABLED
    int size_of_comm;
    int ret = MPI_Comm_size(GetMpiComm(), &size_of_comm);
    assert(ret == MPI_SUCCESS);
    int world_rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &world_rank);

    check(rank < size_of_comm,
          "mpi world rank %d  new rank %d not ok for the size of this "
          "MPI comm %d",
          world_rank, rank, size_of_comm);
#endif
}

void PureCommProcessDetail::AddNewMpiRank(int mpi_rank_new_comm) {
    mpi_new_comm_ranks.push_back(mpi_rank_new_comm);
}

void PureCommProcessDetail::AddOriginPureRank(int pr) {
    pure_origin_comm_ranks.push_back(pr);
}

void PureCommProcessDetail::AddNPureRanks(int n) {
    pure_origin_comm_ranks.reserve(n);
    for (auto r = 0; r < n; ++r) {
        AddOriginPureRank(r);
    }
}

void PureCommProcessDetail::AddNMpiRanksCommWorld(PureThread* const pure_thread,
                                                  int world_size) {
    mpi_new_comm_ranks.reserve(world_size);

    for (auto r = 0; r < world_size; ++r) {
        AddNewMpiRank(pure_thread->MPIRankFromPureRank(r));
    }
}

void PureCommProcessDetail::SetMpiComm(MPI_Comm c) { mpi_comm = c; }

MPI_Comm PureCommProcessDetail::GetMpiComm() const { return mpi_comm; }

// size for ALL pure ranks in this PureComm
int PureCommProcessDetail::Size() const {
    assert(pure_origin_comm_ranks.size() == mpi_new_comm_ranks.size());
    return pure_origin_comm_ranks.size();
}

int PureCommProcessDetail::NumPureRanksThisProcess() const {
    assert(num_pure_ranks_this_process >= 0);
    return num_pure_ranks_this_process;
}

void PureCommProcessDetail::SetNumPureRanksThisProcess(int n) {
    assert(n >= 0);
    num_pure_ranks_this_process = n;
}

int PureCommProcessDetail::OriginCommPureRank(int this_comm_rank) const {
    return pure_origin_comm_ranks[this_comm_rank];
}

int PureCommProcessDetail::CommMpiRank(int new_comm_pure_rank) const {
#if DEBUG_CHECK && MPI_ENABLED
    // fprintf(stderr, "size of mpi entries: %lu\n",
    // mpi_new_comm_ranks.size());

    int size_of_comm;
    int ret = MPI_Comm_size(GetMpiComm(), &size_of_comm);
    assert(ret == MPI_SUCCESS);

    int world_rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &world_rank);

    if (true || new_comm_pure_rank >= mpi_new_comm_ranks.size()) {
        // for (auto i = 0; i < mpi_new_comm_ranks.size(); ++i) {
        //     printf("world mpi rank: %d  looking up new comm pure rank %d: "
        //            "mpi_new_comm_ranks[%d] = %d\n",
        //            world_rank, new_comm_pure_rank, i, mpi_new_comm_ranks[i]);
        // }

        for (auto i = 0; i < mpi_new_comm_ranks.size(); ++i) {
            SanityCheckMpiRank(mpi_new_comm_ranks[i]);
        }
    }
#endif

    check(new_comm_pure_rank < mpi_new_comm_ranks.size(),
          "new_comm_pure_rank = %d   mpi_new_comm_ranks.size = %lu",
          new_comm_pure_rank, mpi_new_comm_ranks.size());
    return mpi_new_comm_ranks[new_comm_pure_rank];
}

int PureCommProcessDetail::NumMpiRanks() const {
    return mpi_new_comm_ranks.size();
}

int PureCommProcessDetail::NumUniqueMpiRanks() const {
    // slow but this shouldn't ever be on the critical path
    return std::set<int>(mpi_new_comm_ranks.begin(), mpi_new_comm_ranks.end())
            .size();
}

PureCommSplitDetails* PureCommProcessDetail::GetSplitDetails() {
    return split_details;
}

// thread leader (comm rank 0) functions
bool PureCommProcessDetail::AllThreadsReached() const {
    // note: the leader never does the increment
    if (barrier_phase == BARRIER_PHASE_COUNT_UP) {
        return (num_threads_reached.load() == NumPureRanksThisProcess() - 1);
    } else {
        return (num_threads_reached.load() == 0);
    }
}

void PureCommProcessDetail::InvertBarrierPhase() {
    barrier_phase = !barrier_phase;
}

// non thread leader function
void PureCommProcessDetail::NonLeaderThreadReached() {
    if (barrier_phase == BARRIER_PHASE_COUNT_UP) {
        ++num_threads_reached;
    } else {
        --num_threads_reached;
    }
}

void PureCommProcessDetail::Barrier(PureThread* const pure_thread,
                                    int               thread_num_in_pure_comm,
                                    seq_type&         thread_barrier_seq,
                                    bool do_work_stealing_while_waiting) {

    const auto root_thread_num = 0;
    hybrid_barrier.Barrier(pure_thread, thread_num_in_pure_comm,
                           NumPureRanksThisProcess(), thread_barrier_seq,
                           root_thread_num, mpi_comm,
                           do_work_stealing_while_waiting);
}

void PureCommProcessDetail::AssertInvarients() const {
#if DEBUG_CHECK

    // mpi ranks are reasonable
#if MPI_ENABLED
    assert(mpi_comm != -1);
    int mpi_comm_size;
    int ret = MPI_Comm_size(mpi_comm, &mpi_comm_size);
    for (const auto& r : mpi_new_comm_ranks) {
        check(r < mpi_comm_size, "MPI rank %d must be less than comm size %d",
              r, mpi_comm_size);
    }
#endif

    assert(num_pure_ranks_this_process != -1);
    assert(mpi_new_comm_ranks.size() == pure_origin_comm_ranks.size());
    // assert(origin_process_detail != nullptr);
#endif
}

// static
void PureCommProcessDetail::RegisterPureCommSplitDetailsMpiType() {
#if MPI_ENABLED
    // make sure to keep in line with PureCommSplitDetails
    // https://www.rookiehpc.com/mpi/docs/mpi_type_create_struct.php
    int            lengths[2]       = {1, 5};
    const MPI_Aint displacements[2] = {0, sizeof(PureComm*)};
    MPI_Datatype   types[2]         = {MPI_AINT, MPI_INT};
    MPI_Type_create_struct(2, lengths, displacements, types,
                           &split_details_mpi_type);
    MPI_Type_commit(&split_details_mpi_type);
#endif
}

// static
void PureCommProcessDetail::DeregisterPureCommSplitDetailsMpiType() {
#if MPI_ENABLED
    // MPI_Type_free(&split_details_mpi_type);
#endif
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

PureComm::PureComm(PureCommProcessDetail* new_process_detail,
                   int rank_in_this_comm, int thread_num_in_process_in_comm_arg)
    : process_detail(new_process_detail), my_rank_this_comm(rank_in_this_comm),
      thread_num_in_process_in_comm(thread_num_in_process_in_comm_arg) {
    new_process_detail->AssertInvarients();
}

PureComm::~PureComm() {
    if (my_rank_this_comm == 0) {
        delete (process_detail);
    }
}

PureComm* PureComm::GetOriginPureComm() const { return origin_pure_comm; }

// returns the pure rank within the origin comm used to create this PureComm
int PureComm::OriginCommMyRank() const {
    return process_detail->pure_origin_comm_ranks[my_rank_this_comm];
}

int PureComm::MyCommWorldPureRank() const {
    const auto world_rank = CommWorldPureRank(my_rank_this_comm);
    assert(world_rank == pure_thread->Get_rank());
    return world_rank;
}

// translates a rank in this PureComm to the corresponding rank in
// PureCommWorld
int PureComm::CommWorldPureRank(int rank_this_comm) const {
    int       curr_rank = rank_this_comm;
    PureComm* curr_comm = const_cast<PureComm*>(this);

    // recurse up to comm world first
    while (curr_comm != pure_thread->GetPureCommWorld()) {
        curr_comm = curr_comm->GetOriginPureComm();
        curr_rank = curr_comm->CommRank();
    }
    assert(curr_comm == pure_thread->GetPureCommWorld());
    return curr_rank;
}

// returns the thread number for the given rank in PURE_COMM_WORLD
int PureComm::CommWorldThreadNum(int rank_this_comm) const {
    return pure_thread->ThreadNumOnNodeFromPureRank(
            CommWorldPureRank(rank_this_comm));
}

int PureComm::MyCommWorldThreadNum() const {
    return CommWorldThreadNum(my_rank_this_comm);
}

MPI_Comm PureComm::GetMpiComm() const { return process_detail->GetMpiComm(); }

// factory for primordial PureComm
// static
PureComm* PureComm::CreatePureCommWorld(PureThread* const pt,
                                        PureProcess*      pure_process,
                                        int               thread_num_in_process,
                                        int world_pure_rank, int world_size,
                                        int num_mpi_processes,
                                        int num_threads_this_process) {

    if (thread_num_in_process == 0) {
        // create the process details
        PureCommProcessDetail* world_process_detail = new PureCommProcessDetail(
                nullptr, pure_process->GetMaxNumThreadsAnyProcess());
        world_process_detail->SetMpiComm(MPI_COMM_WORLD);

        world_process_detail->AddNPureRanks(world_size);
        world_process_detail->AddNMpiRanksCommWorld(pt, world_size);
        world_process_detail->SetNumPureRanksThisProcess(
                num_threads_this_process);

        pure_process->SetPureCommWorldProcessDetail(world_process_detail);
        pure_process->SetPureCommWorldInitialized();
    } else {
        // wait for the process details to be set
        while (pure_process->PureCommWorldInitialized() == false)
            ;
    }

    // Now the process-level details are created, so create my thread-level
    // PureComm
    PureComm* pc = new PureComm(pure_process->GetPureCommWorldProcessDetail(),
                                world_pure_rank, thread_num_in_process);
    pc->SetPureThread(pt);
    pc->SetOriginPureComm(nullptr);

    return pc;
}

int PureComm::GetThreadNumInCommInProcess() const {
    return thread_num_in_process_in_comm;
}

int PureComm::GetNumPureRanksThisProcess() const {
    return process_detail->NumPureRanksThisProcess();
}

int PureComm::Size() const { return process_detail->Size(); }

int PureComm::CommRank() const { return my_rank_this_comm; }

int PureComm::MyCommMpiRank() const {
    return process_detail->CommMpiRank(CommRank());
}

PureCommProcessDetail* PureComm::GetProcessDetails() const {
    return process_detail;
}

void PureComm::Barrier(bool do_work_stealing_while_waiting) {
    process_detail->Barrier(pure_thread, GetThreadNumInCommInProcess(),
                            barrier_sequence_num,
                            do_work_stealing_while_waiting);
}

// you call this with parent pure comm and it returns a new (bound child)
// PureComm
PureComm* PureComm::Split(PureThread* const pure_thread, int color, int key) {

    check(CommRank() < process_detail->Size(),
          "Expected comm rank of this origin pure process (%d) to be less "
          "than the size of this origin pure comm (%d)",
          CommRank(), process_detail->Size());
    check(color >= 0, "color must be nonneg (%d)", color);

    // remember that process_details is the ProcessDetails for the *origin*
    // PureComm (this)
    PureCommSplitDetails* const process_comm_split_details =
            process_detail->GetSplitDetails();
    process_comm_split_details[this->GetThreadNumInCommInProcess()] = {
            this->CommRank(), color, key};

    // Given that every thread inside origin_pure_comm within this process
    // MUST call PureCommSplit, then we can just use rank 0 within
    // origin_pure_comm to determine the leader.
    if (this->GetThreadNumInCommInProcess() == 0) { // leader

        while (process_detail->AllThreadsReached() == false)
            ; // wait until they are all here

        const auto threads_in_origin_comm_in_process =
                process_detail->NumPureRanksThisProcess();
        // // the leader doesn't increment the counter, so that's why we
        // wait
        // // until threads_in_origin_comm_in_process - 1
        // while (process_detail->GetThreadsReached() <
        //        threads_in_origin_comm_in_process - 1)
        //     ; // barrier until process_comm_split_details are initialized
        //     by
        // all threads

        // 2. only the leader thread does the communication
        // const auto origin_comm_size = this->Size();
        const auto world_threads_per_process =
                pure_thread->GetMaxThreadsPerProcess();
        int                   origin_mpi_rank;
        PureCommSplitDetails* global_details;

#if MPI_ENABLED
        // I think I need to only use this path if there's more than one MPI
        // rank in this mpi comm (getmpicomm)

        MPI_Comm_rank(process_detail->GetMpiComm(), &origin_mpi_rank);
        int origin_mpi_size;
        MPI_Comm_size(process_detail->GetMpiComm(), &origin_mpi_size);

        // debugging
        // if (pure_thread->Get_rank() == 7) {
        //     fprintf(stderr,
        //             "[world r%d] ORIGIN MPI SIZE: %d, origin MPI comm: "
        //             "%d\n",
        //             pure_thread->Get_rank(), origin_mpi_size,
        //             process_detail->GetMpiComm());

        //     for (auto t = 0; t < world_threads_per_process; ++t) {
        //         printf("++++ this comm r%d\tTHIS PROCESS DETAILS[%d / %d]
        //         "
        //                "= "
        //                "{%d, "
        //                "%d, "
        //                "%d}\t",
        //                this->CommRank(), t,
        //                threads_in_origin_comm_in_process,
        //                process_comm_split_details[t].origin_pure_rank,
        //                process_comm_split_details[t].color,
        //                process_comm_split_details[t].key);
        //     }
        //     printf("\n");
        // }

        // the size of this should probably be process_detail->Size()
        // we need to set this up to receive world_threads_per_process from
        // as many MPI processes that are participating in this split (i.e.,
        // the number of MPI processes in the origin comm)
        const auto global_details_sz =
                world_threads_per_process * origin_mpi_size;
        PureCommSplitDetails global_details_array[global_details_sz];
        if (origin_mpi_size > 1) {

            // fprintf(stderr,
            //         KLT_RED
            //         "wr%d - Allgather(%p, sendcnt: %d, type, %p, recvcnt:
            //         "
            //         "%d, "
            //         "type, comm  -- origin comm size is %d\n" KRESET,
            //         pure_thread->Get_rank(), process_comm_split_details,
            //         world_threads_per_process, global_details_array,
            //         world_threads_per_process, global_details_sz);

            int ret = MPI_Allgather(
                    process_comm_split_details, world_threads_per_process,
                    split_details_mpi_type, global_details_array,
                    world_threads_per_process, split_details_mpi_type,
                    process_detail->GetMpiComm());
            switch (ret) {
            case MPI_ERR_COMM:
                sentinel("MPI_ERR_COMM");
                break;
            case MPI_ERR_COUNT:
                sentinel("MPI_ERR_COUNT");
                break;
            case MPI_ERR_BUFFER:
                sentinel("MPI_ERR_BUFFER");
                break;
            case MPI_ERR_TYPE:
                sentinel("MPI_ERR_TYPE");
                break;
            }
            check(ret == MPI_SUCCESS, "Allgather return was %d", ret);
            global_details = global_details_array;
        } else {
            // remap global details to the process details, as we only have
            // one MPI process in the origin comm, so don't do a gather.
            global_details = process_comm_split_details;

            // if (pure_thread->Get_rank() == 7) {
            //     for (auto r = 0; r < global_details_sz; ++r) {
            //         printf("this comm r%d\tGLOBAL DETAILS INSIDE[%d] = "
            //                "{%d, "
            //                "%d, "
            //                "%d}\t",
            //                this->CommRank(), r,
            //                global_details[r].origin_pure_rank,
            //                global_details[r].color,
            //                global_details[r].key);
            //     }
            //     printf("\n");
            // }
        }
#else
        const auto global_details_sz = world_threads_per_process * 1;
        origin_mpi_rank              = 0;
        // no need to copy or do the Allgather -- just repoint to keep the
        // code clean
        global_details = process_comm_split_details;
#endif

        if (pure_thread->Get_rank() == 7) {
#define COMM_SPLIT_DEBUG_PRINT 0
#if COMM_SPLIT_DEBUG_PRINT
            for (auto r = 0; r < global_details_sz; ++r) {
                printf("this comm r%d\tGLOBAL DETAILS[%d] = {%d, %d, %d}\t",
                       this->CommRank(), r, global_details[r].origin_pure_rank,
                       global_details[r].color, global_details[r].key);
            }
            printf("\n");
#endif
        }

        // 3. Now all the the ranks have the required data to create the new
        // global MPI communicator and the process-local Pure comm details.
        std::set<int> unique_colors;
        for (auto gt = 0; gt < global_details_sz; ++gt) {
            if (global_details[gt].color >= 0) {
                unique_colors.insert(global_details[gt].color);
            }
        }

        for (auto c : unique_colors) {
            // TODO: only create this if there's > 0 threads in this process
            // with this color
            // STEP 1: create the MPI comm for this color c
            std::vector<int>             origin_mpi_ranks_this_color;
            PureCommProcessDetail* const new_pure_comm_process_details =
                    new PureCommProcessDetail(
                            process_detail,
                            pure_thread->GetMaxThreadsPerProcess());

            for (auto r = 0; r < global_details_sz; ++r) {
                PureCommSplitDetails& details = global_details[r];
                if (details.color == c) {

                    check(c >= 0, "color %d should be nonnegative", c);
                    // Does miniAMR use non-rank-based key ordering with
                    // split?
                    int origin_mpi_rank_for_pure_rank;
                    if (process_detail->GetMpiComm() == MPI_COMM_WORLD) {
                        // this means we are calling split from the "world"
                        // communicator - do we need to special case this???
                        // details.Print();
                        // BUG: ? this should be the proper rank given that
                        // function call.
                        origin_mpi_rank_for_pure_rank =
                                pure_thread->MPIRankFromPureRank(
                                        details.origin_pure_rank);
                    } else {
                        // this branch is probably not working
                        // we are setting an MPI rank
                        origin_mpi_rank_for_pure_rank =
                                process_detail->CommMpiRank(
                                        details.origin_pure_rank);
                    }

                    origin_mpi_ranks_this_color.push_back(
                            origin_mpi_rank_for_pure_rank);
                    new_pure_comm_process_details->AddOriginPureRank(
                            details.origin_pure_rank);
                }
            }

            // if (pure_thread->Get_rank() == 2) {
            //     for (auto i = 0; i < origin_mpi_ranks_this_color.size();
            //     ++i)
            //     {
            //         fprintf(stderr,
            //                 "wr2: origin mpi ranks this color %d [%d] =
            //                 %d\n", c, i, origin_mpi_ranks_this_color[i]);
            //     }
            // }

#if DEBUG_CHECK
            // verify that origin_mpi_ranks_this_color is indeed sorted as
            // the new mpi rank determination algorithm below relies on this
            // being the case
            std::vector<int> sorted_origin_mpi_ranks_this_color =
                    origin_mpi_ranks_this_color;
            std::sort(sorted_origin_mpi_ranks_this_color.begin(),
                      sorted_origin_mpi_ranks_this_color.end());
            assert(origin_mpi_ranks_this_color ==
                   sorted_origin_mpi_ranks_this_color);
#endif

            std::vector<int> new_mpi_ranks;
            int curr_origin_mpi_rank = origin_mpi_ranks_this_color[0];
            int curr_new_mpi_rank    = 0;

            // BUG: threadmap -- looks like this should be rewritten. Actually,
            // it may work as we sort the ranks above. Punt rewrite for now.
            for (auto i = 0; i < origin_mpi_ranks_this_color.size(); ++i) {
                if (origin_mpi_ranks_this_color[i] != curr_origin_mpi_rank) {
                    curr_origin_mpi_rank = origin_mpi_ranks_this_color[i];
                    ++curr_new_mpi_rank;
                }
                new_mpi_ranks.push_back(curr_new_mpi_rank);
            }

            // if (pure_thread->Get_rank() == 2) {
            //     for (auto i = 0; i < new_mpi_ranks.size(); ++i) {
            //         fprintf(stderr, "\twr2: new mpi ranks[%d] = %d\n", i,
            //                 new_mpi_ranks[i]);
            //     }
            // }

            int new_mpi_rank_idx = 0;
            for (auto t = 0; t < global_details_sz; ++t) {
                if (global_details[t].color == c) {
                    new_pure_comm_process_details->AddNewMpiRank(
                            new_mpi_ranks[new_mpi_rank_idx++]);
                }
            }

            std::vector<int> uniq_origin_mpi_ranks_this_color =
                    origin_mpi_ranks_this_color;

            // now we have a vector of all of the mpi ranks for color c, but
            // it may not be unique
            std::sort(uniq_origin_mpi_ranks_this_color.begin(),
                      uniq_origin_mpi_ranks_this_color.end());
            uniq_origin_mpi_ranks_this_color.erase(
                    unique(uniq_origin_mpi_ranks_this_color.begin(),
                           uniq_origin_mpi_ranks_this_color.end()),
                    uniq_origin_mpi_ranks_this_color.end());

            // STEP 2: now, create a group and then a comm
            // Do I have to create a comm if I'm not in it??? Probably not,
            // but ignore for now. (test implmentation in place now)
#if MPI_ENABLED
            MPI_Comm new_mpi_comm;
            if (std::find(uniq_origin_mpi_ranks_this_color.begin(),
                          uniq_origin_mpi_ranks_this_color.end(),
                          origin_mpi_rank) ==
                uniq_origin_mpi_ranks_this_color.end()) {
                new_mpi_comm = MPI_COMM_NULL;

            } else {

                // first just try to do it with no tag at all
                // tsting. but I don't think this will work generally as
                // these could be called with different members out of
                // order. come up with a test to show that.

                // maybe only create if > 1?

                // const auto tag = pure_thread->GetAndIncUniqueCommTag();
                // fprintf(stderr,
                //         KYEL "[wr %d] getting comm for color %d with tag
                //         "
                //              "%d\n" KRESET,
                //         pure_thread->Get_rank(), c,
                //         comm_creation_tag_t0);

                /*

                new_mpi_comm = PureRT::create_mpi_comm_tag(
                        uniq_origin_mpi_ranks_this_color,
                        process_detail->GetMpiComm(), comm_creation_tag_t0);
                */

                // TODO: proxy this call directly (via PureThread)
                new_mpi_comm = pure_thread->GetOrCreateMpiCommFromRanks(
                        uniq_origin_mpi_ranks_this_color,
                        process_detail->GetMpiComm());

                // fprintf(stderr,
                //         KGRN "  [wr %d] Finished creating comm (%d) with
                //         tag
                //         "
                //              "%d\n" KRESET,
                //         pure_thread->Get_rank(), comm_creation_tag_t0);
            }

            // we always increment the tag for each color, even if we are
            // not participating in it. comm_creation_tag_t0++;
            new_pure_comm_process_details->SetMpiComm(new_mpi_comm);
#endif

            // determine the number of pure ranks in this process
            int pure_ranks_this_process_this_comm = 0;
            // world_threads_per_process is the max number of threads that
            // could be in this comm in this process
            for (auto t = 0; t < world_threads_per_process; ++t) {
                if (process_comm_split_details[t].color == c) {
                    ++pure_ranks_this_process_this_comm;
                }
            }
            new_pure_comm_process_details->SetNumPureRanksThisProcess(
                    pure_ranks_this_process_this_comm);

            // determine ranks for each color
            int max_color = 0;
            for (auto _c : unique_colors) {
                if (_c > max_color) {
                    max_color = _c;
                }
            }
            int rank_counter_for_color[max_color + 1]; // counter for each
                                                       // color
            for (auto i = 0; i <= max_color; ++i) {
                rank_counter_for_color[i] = 0;
            }

            // loop through, setting the color and new MPI rank
            // TODO: deal with key sort
            for (auto r = 0; r < global_details_sz; ++r) {
                PureCommSplitDetails& details    = global_details[r];
                const auto            this_color = details.color;
                if (this_color >= 0) {
                    // because we don't know how many threads per MPI rank
                    // will be participating in each allgather, we have to
                    // assume they all do and just filter out empty
                    // (sentinel) entries.
                    details.new_pure_rank =
                            rank_counter_for_color[this_color]++;
                }
            }

            // determine thread num in process in comm for each color
            const auto num_colors = max_color + 1;
            int        thread_counter_for_color[num_colors]; // counter for each
            memset(thread_counter_for_color, 0, sizeof(int) * num_colors);

            // now, loop through process_comm_split_details and set the
            // resulting_pure_comm field for all entries that have this
            // color. Each thread needs their own copy of their PureComm as
            // it contains the rank for them.
            // assert all are set
            // global_details now contains the finalized PureComm. But, each
            // thread needs its own copy with some customization...
            for (auto t = 0; t < threads_in_origin_comm_in_process; ++t) {
                if (process_comm_split_details[t].color == c) {

                    // find corresponding entry in global details
                    auto offset_into_global =
                            world_threads_per_process * origin_mpi_rank;

                    // this doesn't work for the general case!!!!!!! offset
                    // into global is hard to figure out for the general
                    // scheme. Come up with a general scheme for this.

                    const auto& corresponding_global_details =
                            global_details[offset_into_global + t];

                    const auto thread_num_within_process_within_comm =
                            thread_counter_for_color[c]++;

                    // const auto comm_creation_tag_t0_child =
                    //         comm_creation_tag_t0 +
                    //         PureComm::comm_creation_tag_child_delta;
                    PureComm* const thread_pure_comm = new PureComm(
                            new_pure_comm_process_details,
                            corresponding_global_details.new_pure_rank,
                            thread_num_within_process_within_comm);

                    // fprintf(stderr,
                    //         "just created new pure comm %p with origin
                    //         pure " "comm %p\n", thread_pure_comm, this);

                    // TODO: make sure you're storing the new comm in the
                    // proper place. Should it be in the
                    // process_comm_split_details (I think so!) or down a
                    // level? I think down a level is for the next split,
                    // using the new comm as a container for that.

                    process_comm_split_details[t].resulting_pure_comm =
                            thread_pure_comm;

                    // fprintf(stderr,
                    //         KGRN
                    //         "COLOR: %d: origin mpi rank: %d: new comm
                    //         rank %d, " "thread in process in comm %d\n"
                    //         KRESET, c, origin_mpi_rank,
                    //         corresponding_global_details.new_pure_rank,
                    //         thread_num_within_process_within_comm);
                }
            } // ends thread copy loop
        }     // ends for each color

        for (auto t = 0; t < threads_in_origin_comm_in_process; ++t) {
            process_comm_split_details[t].AssertInvarients(t);
        }

        // reset barrier and let other threads go forward
        // assert(process_detail->GetThreadsReached() <=
        //        threads_in_origin_comm_in_process - 1);
        // process_detail->ResetThreadsReached();
        process_detail->InvertBarrierPhase();
    } else {
        // non-leaders ////////////////////////////////////////////////////
        const auto curr_barrier_phase = process_detail->barrier_phase.load();
        process_detail->NonLeaderThreadReached();
        // process_detail->IncThreadsReached(); // barrier
        // if (process_detail->GetMpiComm() == MPI_COMM_WORLD) {
        //     check(comm_creation_tag_t0 ==
        //     PureComm::comm_creation_tag_unused,
        //           "comm_creation_tag_t0 is %d",
        //           comm_creation_tag_t0); // only t0 should use this
        // }

        while (process_detail->barrier_phase == curr_barrier_phase)
            ; // wait until leader changes the barrier phase
    }

    // Finally, set the PureThread field in the newly-creataed PureComm
    PureComm* const new_pure_comm =
            process_comm_split_details[this->GetThreadNumInCommInProcess()]
                    .resulting_pure_comm;
    new_pure_comm->SetPureThread(pure_thread);
    new_pure_comm->SetOriginPureComm(this);

    // finish up
    return new_pure_comm;
    // END LOCK
    // ///////////////////////////////////////////////////////////////////////////////
}
