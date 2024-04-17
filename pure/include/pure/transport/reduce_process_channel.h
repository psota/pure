// Author: James Psota
// File:   reduce_process_channel.h 

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

#ifndef REDUCE_PROCESS_CHANNEL_H
#define REDUCE_PROCESS_CHANNEL_H

#include "pure/common/pure_rt_enums.h"
#include "pure/runtime/pure_thread.h"
#include "pure/support/helpers.h"
#include "pure/transport/mpi_pure_helpers.h"
#include "pure/transport/reduce_channel.h"
#include "pure/transport/reduce_process_channel.h"
#include <algorithm>
#include <atomic>
#include <optional>
#include <vector>

class ReduceProcessChannel {

  public:
    ReduceProcessChannel(int count_arg, int datatype_bytes_arg,
                         MPI_Datatype datatype_arg,
                         int mpi_rank_for_root_in_pure_comm, int size_bytes_arg,
                         int  root_pure_rank_in_comm_arg,
                         int  pure_ranks_this_comm_this_process,
                         bool is_multiprocess_arg, bool is_root_process_arg,
                         MPI_Op op_arg);
    ~ReduceProcessChannel();

    // must be called by each of the associated Reducehannels
    void BindReduceChannel(int                  thread_num_in_process_in_comm,
                           ReduceChannel* const rc);
    void WaitForInitialization(PureThread* pure_thread);
    void ReduceBlocking(PureThread*           pure_thread,
                        PureComm const* const thread_pure_comm,
                        buffer_t src_buf, buffer_t recv_buf,
                        uint64_t& thread_leader_ticket_num, bool& thread_sense,
                        bool*               is_process_leader,
                        std::optional<bool> do_allreduce);

  private:
    // read-only members
    alignas(CACHE_LINE_BYTES) ReduceChannel** bound_chans;
    const buffer_t __restrict process_reduce_buf = nullptr;
    const int          count;
    const int          datatype_bytes;
    const MPI_Datatype datatype;
    const MPI_Op       op;
    const int          mpi_rank_for_root_in_pure_comm;
    const int          root_pure_rank_in_comm;
    const int          pure_ranks_this_comm_this_process;
    const int          size_bytes;
    int                cache_lines;
    int                num_threads_computing;
    int                lines_per_thread;
    const bool         is_multiprocess;
    const bool         is_root_process;

    // synchronization members
    alignas(CACHE_LINE_BYTES) std::atomic<uint64_t> process_leader_ticket_num =
            0;
    std::atomic<int>  num_threads_done             = 0;
    std::atomic<int>  num_non_leaders_totally_done = 0;
    std::atomic<int>  num_bound_channels           = 0;
    std::atomic<bool> channel_ready_for_use;

    char end_pad[CACHE_LINE_BYTES -
                 (sizeof(std::atomic<uint64_t>) + 3 * sizeof(std::atomic<int>) +
                  sizeof(std::atomic<bool>))];

    ////////////////////////////////////////////////////////
    void InitializeBuffer();
    bool ElectProcessLeader(int      calling_pure_rank,
                            uint64_t thread_leader_ticket_num);
    void WaitForProcessReductionsToComplete(PureThread* pure_thread);
    void ResetChannelForReuse();
    void MarkChunkDone();
    void WaitForNonLeadersToWrapup(PureThread* pure_thread);
    void WrapupMyReduceChannel(int);
    void ResetLeaderChannel(int);
    void NonLeaderFinalization(int);

    template <typename T>
    void ReduceMyChunks(int thread_num_in_process_in_comm,
                        buffer_t __restrict src_buf,
                        buffer_t __restrict recv_buf) {

        if (cache_lines < pure_ranks_this_comm_this_process) {
            assert(lines_per_thread == 1);
            // more threads than cache lines; just the lower numbered threads
            // get work to do
            if (thread_num_in_process_in_comm >= cache_lines) {
                // STEP 2: exit early if I don't own any chunks (because the
                // buffer is small) there's no work for me to do for example, if
                // there are 4 cache lines worth of work, and 8 worker threads,
                // ranks 0-3 each do a single cache line and ranks 4-7 do none
                MarkChunkDone();
                // fprintf(stderr,
                //         KYEL "\tthread %d leaving early. num_threads_done = "
                //              "%d\n" KRESET,
                //         thread_num_in_process_in_comm,
                //         num_threads_done.load());
                return;
            }
        }

        // STEP 3: calculate the indicies for this chunk, assuming the buffer is
        // aligned
        assert_cacheline_aligned(process_reduce_buf);
        int low_idx = (thread_num_in_process_in_comm * lines_per_thread) *
                      CACHE_LINE_BYTES / datatype_bytes;

        // high_idx is inclusive
        int high_idx;

        // the last *active* thread should go all the way until the end
        if (((cache_lines < pure_ranks_this_comm_this_process) &&
             (thread_num_in_process_in_comm == cache_lines - 1)) ||
            (thread_num_in_process_in_comm ==
             pure_ranks_this_comm_this_process - 1)) {
            high_idx = count - 1;
        } else {
            high_idx =
                    (((thread_num_in_process_in_comm + 1) * lines_per_thread) *
                     CACHE_LINE_BYTES / datatype_bytes) -
                    1;
        }
        ///////////////////////////

        T* reduce_buf_start = &static_cast<T*>(process_reduce_buf)[low_idx];
        assert_cacheline_aligned(reduce_buf_start);

        const T* my_buf_start = &static_cast<T*>(src_buf)[low_idx];

        // STEP 3: copy in my data for this chunk
        const int num_elts = high_idx - low_idx + 1;
        MEMCPY_IMPL(reinterpret_cast<void* __restrict>(reduce_buf_start),
                    reinterpret_cast<const void* __restrict>(my_buf_start),
                    num_elts * datatype_bytes);

        // fprintf(stderr,
        //         "B: thread %d doing reduction [%d-%d]. count=%d
        //         cache_lines=%d " "lines_per_thread=%d  reduction_buf[1] =
        //         %d\n", thread_num_in_process_in_comm, low_idx, high_idx,
        //         count, cache_lines, lines_per_thread,
        //         static_cast<T*>(process_reduce_buf)[1]);

        // STEP 4: loop through all the other threads in this process and reduce
        // them
        // we want static initialization, so just make a bigger array than we
        // may need
        assert(pure_ranks_this_comm_this_process <= 64);
        bool thread_done[64] = {false};

        // MEMSET_IMPL(thread_done, 0,
        //             pure_ranks_this_comm_this_process * sizeof(bool));
        thread_done[thread_num_in_process_in_comm] = true;
        bound_chans[thread_num_in_process_in_comm]->MarkDoneConsumption();

        int threads_remain = pure_ranks_this_comm_this_process - 1;
        while (threads_remain > 0) {
            // approach: keep looping through all of the attached threads
            for (auto i = 0; i < pure_ranks_this_comm_this_process; ++i) {

                if (thread_done[i]) {
                    continue;
                }

                assert(bound_chans[i] != nullptr);
                const T* __restrict curr_buf =
                        static_cast<const T*>(bound_chans[i]->GetBuf());
                if (curr_buf != nullptr) {
                    ReduceChunk<T>(curr_buf, low_idx, high_idx);
                    bound_chans[i]->MarkDoneConsumption();
                    thread_done[i] = true;
                    --threads_remain;
                }
            } // for
        }     // while

        MarkChunkDone();
    }

    template <typename T>
    void ReduceChunk(const T* __restrict src_buf, int low_idx, int high_idx) {

        T* __restrict prb = static_cast<T*>(
                __builtin_assume_aligned(process_reduce_buf, CACHE_LINE_BYTES));

        switch (op) {
        case MPI_SUM: {
            for (auto i = low_idx; i <= high_idx; ++i) {
                prb[i] += src_buf[i];
            }
            break;
        }
        case MPI_MAX: {
            assert(datatype == MPI_INT ||
                   datatype == MPI_DOUBLE); // only this is supported currently
                                            // given how InitializeBuffer works
            for (auto i = low_idx; i <= high_idx; ++i) {
                if (src_buf[i] > prb[i]) {
                    prb[i] = src_buf[i];
                }
            }
            break;
        }
        case MPI_MIN: {
            assert(datatype == MPI_INT ||
                   datatype == MPI_DOUBLE); // only this is supported currently
                                            // given how InitializeBuffer works
            for (auto i = low_idx; i <= high_idx; ++i) {
                if (src_buf[i] < prb[i]) {
                    prb[i] = src_buf[i];
                }
            }
            break;
        }
        default: {
            sentinel("MPI_Op %d not currently supported in "
                     "ReduceProcessChannel.",
                     op);
        }
        }
    }
};

#endif