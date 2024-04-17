// Author: James Psota
// File:   bcast_process_channel.cpp 

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

#include "pure/transport/bcast_process_channel.h"
#include "mpi.h"
#include "pure/common/pure_rt_enums.h"
#include "pure/runtime/pure_comm.h"
#include "pure/runtime/pure_thread.h"
#include "pure/support/helpers.h"
#include "pure/support/sp_bcast_queue.h"
#include "pure/transport/mpi_pure_helpers.h"
#include <atomic>

// test this!
#define BCAST_QUEUE_ENTRIES 32

// do queue up to MAX_BYTES_FOR_INTRANODE_QUEUE_STYLE and direct copy above
// that.

BcastProcessChannel::BcastProcessChannel(
        int count_arg, MPI_Datatype datatype_arg, int root_mpi_rank_in_comm_arg,
        int size_bytes_arg, int root_pure_rank_in_pure_comm_arg,
        int  num_threads_this_process_comm_arg,
        bool pure_comm_is_multiprocess_arg, bool is_root_process_arg,
        MPI_Comm mpi_comm_arg)
    : count(count_arg), datatype(datatype_arg),
      root_mpi_rank_in_comm(root_mpi_rank_in_comm_arg),
      root_pure_rank_in_pure_comm(root_pure_rank_in_pure_comm_arg),
      num_threads_this_process_comm(num_threads_this_process_comm_arg),
      size_bytes(size_bytes_arg),
      pure_comm_is_multiprocess(pure_comm_is_multiprocess_arg),
      is_root_process(is_root_process_arg), mpi_comm(mpi_comm_arg),
      queue(BCAST_QUEUE_ENTRIES, size_bytes_arg,
            num_threads_this_process_comm_arg - 1),
      done_consuming_flag(START_CONSUMED_FLAG) {

    check(num_threads_this_process_comm > 0,
          "num_threads_this_process_comm is %d", num_threads_this_process_comm);

    if (num_threads_this_process_comm > 1) {
        // this is only used in multithreaded (non-PureSingleThreaded) mode
        done_consuming_flags =
                jp_memory_alloc<1, CACHE_LINE_BYTES, 0, PaddedAtomicFlag>(
                        sizeof(PaddedAtomicFlag) *
                        (num_threads_this_process_comm - 1));

        for (auto i = 0; i < num_threads_this_process_comm - 1; ++i) {
            // the leader is always thread zero, so this gets indexed using the
            // non-leaders thread num minus one -- fixme
            done_consuming_flags[i].flag.store(START_NOT_CONSUMED_FLAG);
        }
    }
}

BcastProcessChannel::~BcastProcessChannel() { // free(thread_data);
    if (num_threads_this_process_comm > 1) {
        free(done_consuming_flags);
    }
}

// must be called by each of the associated BcastChannels
void BcastProcessChannel::BindBcastChannel(BcastChannel* const bc) {
    bc->SetBcastProcessChannel(this);
}

// calling_pure_rank_in_pure_comm -- change name

void BcastProcessChannel::BcastBlocking(
        PureThread* pure_thread, int calling_pure_rank_in_pure_comm,
        int thread_num_in_pure_comm, buffer_t buf,
        uint32_t& my_consumer_read_index, uint32_t& thread_seq,
        std::optional<bool> force_process_leader,
        std::optional<bool> skip_cross_process_bcast) {

    ++thread_seq;

    // 1. if multiprocess, do the MPI bcast
    if (pure_comm_is_multiprocess) {
        assert(MPI_ENABLED);
        /////// MULTI-PROCESS ////////////////////////////////////

        bool is_process_leader;
        if (force_process_leader.has_value()) {
            // we assume exactly one thread in the process has this set to
            // true
            is_process_leader = force_process_leader.value();
        } else {
            // 1. Elect leader for this process if no forced leader is
            // passed in
            is_process_leader = (thread_num_in_pure_comm == 0);
        }

        if (is_process_leader) {

            // only thread 0 is supported for now as the process leader.
            // change this in for loops and data structures if necessary.
            assert(thread_num_in_pure_comm == 0);

            if (!skip_cross_process_bcast.has_value() ||
                skip_cross_process_bcast.value() == false) {

                const auto ret = MPI_Bcast(buf, count, datatype,
                                           root_mpi_rank_in_comm, mpi_comm);
                check(ret == MPI_SUCCESS, "ret was %d", ret);
            }

            if (num_threads_this_process_comm > 1) {
                LeaderQueueBcast(pure_thread, buf);
            }
        } else {
            if (num_threads_this_process_comm > 1) {
                NonLeaderQueueRecvBcast(pure_thread, buf,
                                        thread_num_in_pure_comm,
                                        my_consumer_read_index);
            }
        }
    } else {
        /////// SINGLE-PROCESS ////////////////////////////////////
        if (calling_pure_rank_in_pure_comm == root_pure_rank_in_pure_comm &&
            num_threads_this_process_comm > 1) {
            // fprintf(stderr, "r%d here 2\n", pure_thread->Get_rank());
            if (DoQueueStyleBcast()) {
                LeaderQueueBcast(pure_thread, buf);
            } else {
                LeaderDirectCopyBcast(pure_thread, thread_seq, buf);
            }
        } else { // not leader / root -- just wait until the data is valid
                 // fprintf(stderr, "r%d here 2\n",
                 // pure_thread->Get_rank());
            if (DoQueueStyleBcast()) {
                NonLeaderQueueRecvBcast(pure_thread, buf,
                                        thread_num_in_pure_comm,
                                        my_consumer_read_index);
            } else {
                NonLeaderDirectCopyBcast(pure_thread, thread_seq, buf,
                                         thread_num_in_pure_comm);
            }
        }
    }
}

// queue versions
void BcastProcessChannel::LeaderQueueBcast(
        PureThread* pure_thread, void const* const __restrict src_buf) {

    queue.push_bcast_data(src_buf);
}

void BcastProcessChannel::NonLeaderQueueRecvBcast(
        PureThread* pure_thread, void* const __restrict dest_buf,
        int thread_num_in_pure_comm, uint32_t& my_read_index) {

    queue.copy_out_bcast_data(dest_buf, pure_thread->Get_rank(),
                              thread_num_in_pure_comm, my_read_index);
}

/////////////////////////
bool BcastProcessChannel::DoQueueStyleBcast() {
    return (size_bytes < MAX_BYTES_FOR_INTRANODE_QUEUE_STYLE);
}

// direct copy versions
void BcastProcessChannel::LeaderDirectCopyBcast(
        PureThread* pure_thread, uint32_t thread_seq,
        void* const __restrict src_buf) {

    assert(src_buf != nullptr);
    read_only_process_leader_buf.store(src_buf, std::memory_order_relaxed);
    bcast_data_ready_seq.store(thread_seq, std::memory_order_release);

    // wait until all have received
    for (auto t = 0; t < num_threads_this_process_comm - 1; ++t) {
        while (done_consuming_flags[t].flag.load(std::memory_order_acquire) !=
               done_consuming_flag) {
            x86_pause_instruction();
        }
    }

    done_consuming_flag ^= 1; // invert for next time
}

void BcastProcessChannel::NonLeaderDirectCopyBcast(
        PureThread* const pure_thread, uint32_t thread_seq,
        void* const __restrict dest_buf, int    thread_num_in_pure_comm) {

    // 1. wait for buf pointer from leader
    while (bcast_data_ready_seq.load(std::memory_order_acquire) != thread_seq) {
        MAYBE_WORK_STEAL(pure_thread);
        x86_pause_instruction();
    }

    read_only_process_leader_buf.load(std::memory_order_acquire);

    // 2. copy
    MEMCPY_IMPL(dest_buf,
                read_only_process_leader_buf.load(std::memory_order_acquire),
                size_bytes);

    // 3. indicate done copying
    const auto prev_flag =
            done_consuming_flags[thread_num_in_pure_comm - 1].flag.fetch_xor(1);
    assert(prev_flag == 0 || prev_flag == 1);
}
