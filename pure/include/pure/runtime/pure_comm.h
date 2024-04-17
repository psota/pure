// Author: James Psota
// File:   pure_comm.h 

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

#ifndef PURE_COMM_H
#define PURE_COMM_H

#include "mpi.h"
#include "pure/common/experimental/hybrid_barrier.h"
#include "pure/support/colors.h"
#include "pure/support/helpers.h"
#include "pure/support/zed_debug.h"
#include <atomic>

static MPI_Datatype split_details_mpi_type;

class PureComm;
class PureProcess;
class PureThread;

/*
 * Future Improvements
 * 1. deallocate extra comms, groups, datatype
   2. Implmeent color "undefined" : A process may supply the color value
 MPI_UNDEFINED, in which case newcomm returns MPI_COMM_NULL. 3.
*/

struct PureCommSplitDetails;
class PureCommProcessDetail;

// internal bookkeeping class
struct PureCommSplitDetails {

    static const int split_details_uninit_val = -2;

    // make sure to keep register_pure_comm_split_details_mpi_type consistent
    // with this

    // only filled in by the leader in the initialization of a PureComm, tnum 0
    // (?)
    PureComm* resulting_pure_comm = nullptr;

    // I think this one may not be used
    int origin_mpi_rank  = split_details_uninit_val;
    int origin_pure_rank = split_details_uninit_val;
    int new_pure_rank    = split_details_uninit_val;
    int color            = split_details_uninit_val;
    int key              = split_details_uninit_val;

    PureCommSplitDetails()
        : origin_mpi_rank(split_details_uninit_val),
          origin_pure_rank(split_details_uninit_val),
          new_pure_rank(split_details_uninit_val),
          color(split_details_uninit_val), key(split_details_uninit_val) {}

    PureCommSplitDetails(int pr, int c, int k)
        : origin_pure_rank(pr), color(c), key(k) {}

    void AssertInvarients(int flag) const {
        if (color == split_details_uninit_val) {
            // color is a field that's always set (both in the local and global
            // data structures)
            assert(new_pure_rank == split_details_uninit_val);
            assert(origin_pure_rank == split_details_uninit_val);
            assert(color == split_details_uninit_val);
            assert(key == split_details_uninit_val);
        } else {
            // check(origin_mpi_rank >= 0, "origin_mpi_rank is %d",
            // origin_mpi_rank);
            check(origin_pure_rank >= 0, "origin_pure_rank is %d",
                  origin_pure_rank);
            // check(new_pure_rank >= 0, "new_pure_rank is %d", new_pure_rank);
            check(color >= 0, "color is %d", color);
            check(key >= 0, "key is %d", key);

            if (resulting_pure_comm == nullptr) {
                Print();
                check(resulting_pure_comm != nullptr,
                      "null pure comm. color is %d, flag is %d", color, flag);
            }
        }
    }

    void Print() const {
        fprintf(stderr,
                KGRN "PureCommSplitDetails::Print: origin_pure_rank: %d  "
                     "color: %d  key: "
                     "%d  --> new_pure_rank: %d\n" KRESET,
                origin_pure_rank, color, key, new_pure_rank);
    }
};

#define BARRIER_PHASE_COUNT_UP true
#define BARRIER_PHASE_COUNT_DOWN true

class PureCommProcessDetail {
    friend PureComm;

  public:
    PureCommProcessDetail(PureCommProcessDetail* origin_process_detail_arg,
                          int                    max_threads_per_process);
    ~PureCommProcessDetail();
    void AddNewMpiRank(int mpi_rank_new_comm);
    void AddOriginPureRank(int pr);
    void AddNPureRanks(int n);
    void AddNMpiRanksCommWorld(PureThread* const pure_thread, int world_size);

    void     SetMpiComm(MPI_Comm c);
    MPI_Comm GetMpiComm() const;
    int      Size() const;
    int      NumPureRanksThisProcess() const;
    void     SetNumPureRanksThisProcess(int n);
    int      OriginCommPureRank(int this_comm_rank) const;
    int      CommMpiRank(int new_comm_pure_rank) const;
    int      NumMpiRanks() const;
    int      NumUniqueMpiRanks() const;

    PureCommSplitDetails* GetSplitDetails();

    bool AllThreadsReached() const;
    void InvertBarrierPhase();
    void NonLeaderThreadReached();

    // barrier helpers
    inline int NumThreadsReachedBarrier() {
        return num_threads_reached_barrier.load(std::memory_order_acquire);
    }
    inline void IncNumThreadsReachedBarrier() { ++num_threads_reached_barrier; }
    inline void ResetNumThreadsReachedBarrier() {
        num_threads_reached_barrier.store(0, std::memory_order_release);
    }
    inline bool CommBarrierSense() const {
        return comm_barrier_sense.load(std::memory_order_acquire);
    }
    inline void SetBarrierSense(bool sense) {
        comm_barrier_sense.store(sense, std::memory_order_release);
    }

    void Barrier(PureThread* const pure_thread, int thread_num_in_pure_comm,
                 seq_type& thread_barrier_seq,
                 bool      do_work_stealing_while_waiting);

    // end barrier helpers

    static void RegisterPureCommSplitDetailsMpiType();
    static void DeregisterPureCommSplitDetailsMpiType();

  private:
    // gives all instantiations of a PureCommProcessDetail a unique id, which
    // can be used in the application or elsewhere to disambiguate between
    // communicators within a process.
    static atomic<int> internal_id_process_cnt;

    std::vector<int>       mpi_new_comm_ranks;
    std::vector<int>       pure_origin_comm_ranks;
    PureCommProcessDetail* origin_process_detail       = nullptr;
    MPI_Comm               mpi_comm                    = -1;
    int                    num_pure_ranks_this_process = -1;
    const int              internal_id                 = -1;

    alignas(CACHE_LINE_BYTES) PureCommSplitDetails* split_details;
    std::atomic<int>  num_threads_reached         = 0;
    std::atomic<bool> barrier_phase               = BARRIER_PHASE_COUNT_UP;
    std::atomic<int>  num_threads_reached_barrier = 0;
    std::atomic<bool> comm_barrier_sense          = true;

    alignas(CACHE_LINE_BYTES) HybridBarrier hybrid_barrier;

    //
    void SanityCheckMpiRank(int rank) const;
    void AssertInvarients() const;
};

class PureComm {
  public:
    PureComm(PureCommProcessDetail* pd, int rank_in_this_comm,
             int thread_num_in_process_in_comm_arg);
    ~PureComm();

    static PureComm*       CreatePureCommWorld(PureThread* const,
                                               PureProcess* const pure_process,
                                               int thread_num_in_process,
                                               int world_pure_rank, int world_size,
                                               int num_mpi_processes,
                                               int num_threads_this_process);
    int                    CommRank() const;
    int                    MyCommWorldPureRank() const;
    int                    CommWorldPureRank(int) const;
    int                    MyCommWorldThreadNum() const;
    int                    CommWorldThreadNum(int rank_this_comm) const;
    int                    GetThreadNumInCommInProcess() const;
    int                    GetNumPureRanksThisProcess() const;
    int                    Size() const;
    MPI_Comm               GetMpiComm() const;
    int                    OriginCommMyRank() const;
    int                    MyCommMpiRank() const;
    PureCommProcessDetail* GetProcessDetails() const;
    void                   Barrier(bool do_work_stealing_while_waiting = true);
    PureComm* Split(PureThread* const pure_thread, int color, int key);
    PureComm* GetOriginPureComm() const;

    // simple inline functions
    // these have to be called by each individual thread, as each PureComm is
    // specific to a given PureThread.
    inline void SetPureThread(PureThread* pt) { pure_thread = pt; }
    inline void SetOriginPureComm(PureComm* pc) { origin_pure_comm = pc; }
    inline auto CommMpiRank(int rank_in_this_comm) const {
        return process_detail->CommMpiRank(rank_in_this_comm);
    }
    inline int ProcessInternalId() const { return process_detail->internal_id; }

  private:
    PureCommProcessDetail* const process_detail; // process-level metadata

    // TODO: actually initialize this, ideally in the constructor
    PureComm*   origin_pure_comm     = nullptr;
    PureThread* pure_thread          = nullptr;
    seq_type    barrier_sequence_num = 0;
    const int   my_rank_this_comm;
    const int   thread_num_in_process_in_comm;
};

#endif