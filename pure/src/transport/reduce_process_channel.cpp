// Author: James Psota
// File:   reduce_process_channel.cpp 

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


#include "pure/transport/reduce_process_channel.h"
#include "pure.h"
#include "pure/common/pure_rt_enums.h"
#include "pure/runtime/pure_comm.h"
#include "pure/runtime/pure_thread.h"
#include "pure/support/helpers.h"
#include "pure/transport/mpi_pure_helpers.h"
#include "pure/transport/reduce_channel.h"
#include <algorithm>
#include <atomic>
#include <float.h>
#include <optional>
#include <vector>

ReduceProcessChannel::ReduceProcessChannel(
        int count_arg, int datatype_bytes_arg, MPI_Datatype datatype_arg,
        int mpi_rank_for_root_in_pure_comm_arg, int size_bytes_arg,
        int root_pure_rank_in_comm_arg,
        int pure_ranks_this_comm_this_process_arg, bool is_multiprocess_arg,
        bool is_root_process_arg, MPI_Op op_arg)
    : process_reduce_buf(
              jp_memory_alloc<1, CACHE_LINE_BYTES, false>(size_bytes_arg)),
      count(count_arg), datatype_bytes(datatype_bytes_arg),
      datatype(datatype_arg), op(op_arg),
      mpi_rank_for_root_in_pure_comm(mpi_rank_for_root_in_pure_comm_arg),
      root_pure_rank_in_comm(root_pure_rank_in_comm_arg),
      pure_ranks_this_comm_this_process(pure_ranks_this_comm_this_process_arg),
      size_bytes(size_bytes_arg), is_multiprocess(is_multiprocess_arg),
      is_root_process(is_root_process_arg) {

    bound_chans = new ReduceChannel*[pure_ranks_this_comm_this_process];
    memset(bound_chans, 0,
           pure_ranks_this_comm_this_process * sizeof(ReduceChannel*));
    InitializeBuffer();

    // STEP 1: get chunk boundaries (on cache line boundaries except for the
    // first and last line)
    cache_lines = size_bytes / CACHE_LINE_BYTES;
    if (size_bytes % CACHE_LINE_BYTES != 0) {
        cache_lines += 1;
    }

    lines_per_thread = cache_lines / pure_ranks_this_comm_this_process;
    if (lines_per_thread == 0) {
        lines_per_thread = 1;
    }
    num_threads_computing =
            std::min(cache_lines, pure_ranks_this_comm_this_process);
    channel_ready_for_use.store(true);
}

ReduceProcessChannel::~ReduceProcessChannel() {
    free(process_reduce_buf);
    delete[] bound_chans;
}

// must be called by each of the associated Reducehannels
void ReduceProcessChannel::BindReduceChannel(int thread_num_in_process_in_comm,
                                             ReduceChannel* const rc) {
    check(thread_num_in_process_in_comm < pure_ranks_this_comm_this_process,
          "thread_num_in_process_in_comm: %d  "
          "pure_ranks_this_comm_this_process: %d",
          thread_num_in_process_in_comm, pure_ranks_this_comm_this_process);
    rc->SetReduceProcessChannel(this);

    if (bound_chans[thread_num_in_process_in_comm] != nullptr) {
        fprintf(stderr,
                "ERROR: thread num in process in comm %d trying to bind "
                "ReduceChannel %p "
                "to ReduceProcessChannel %p, but it's already bound. Check to "
                "see if the channel endpoint tag (CET) is used multiple times "
                "in the application.\n",
                thread_num_in_process_in_comm, rc, this);
        PURE_ABORT;
    }
    bound_chans[thread_num_in_process_in_comm] = rc;
    ++num_bound_channels;
}

void ReduceProcessChannel::WaitForInitialization(PureThread* pure_thread) {
    while (num_bound_channels.load(std::memory_order_acquire) <
           pure_ranks_this_comm_this_process) {
        MAYBE_WORK_STEAL(pure_thread);
    }
}

void ReduceProcessChannel::ReduceBlocking(
        PureThread* pure_thread, PureComm const* const thread_pure_comm,
        buffer_t src_buf, buffer_t recv_buf, uint64_t& thread_leader_ticket_num,
        bool& thread_sense, bool* is_process_leader_out,
        std::optional<bool> do_allreduce) {

    // TODO: convert this to use per-thread gather/release

    // STEP 1: wait until previous reduce on this channel is done
    // wait until previous use of this channel is done.
    while (channel_ready_for_use.load(std::memory_order_acquire) !=
           thread_sense) {
        MAYBE_WORK_STEAL(pure_thread);
    }

    // STEP 2: reduce my chunk of the buffer for this process

    // TODO: likely change to rank in comm
    // const auto calling_pure_rank_in_comm = pure_thread->Get_rank();
    const auto calling_pure_rank_in_comm =
            pure_thread->Get_rank(thread_pure_comm);
    if (thread_pure_comm == PURE_COMM_WORLD) {
        assert(calling_pure_rank_in_comm == pure_thread->Get_rank());
    }

    const auto thread_num_in_process_in_comm =
            thread_pure_comm->GetThreadNumInCommInProcess();

    switch (datatype) {
    case MPI_INT:
        ReduceMyChunks<int>(thread_num_in_process_in_comm, src_buf, recv_buf);
        break;
    case MPI_FLOAT:
        ReduceMyChunks<float>(thread_num_in_process_in_comm, src_buf, recv_buf);
        break;
    case MPI_DOUBLE:
        ReduceMyChunks<double>(thread_num_in_process_in_comm, src_buf,
                               recv_buf);
        break;
    case MPI_LONG_LONG_INT:
        ReduceMyChunks<long long int>(thread_num_in_process_in_comm, src_buf,
                                      recv_buf);
        break;
    default:
        sentinel("Unsupported MPI_Datatype %d", datatype);
    }

    // STEP 4: finalize reduction into buf
    if (is_multiprocess) {
        const bool is_process_leader = ElectProcessLeader(
                calling_pure_rank_in_comm, thread_leader_ticket_num);

        if (is_process_leader_out != nullptr) {
            // this is for use in all reduce, where we want the same thread to
            // be the leader in the bcast.
            *is_process_leader_out = is_process_leader;
        }

        // STEP 3b: do the MPI reduce (if necessary)
        // Do MPI_Ireduce to allow for work stealing as a second step after
        // working
        if (is_process_leader) {
            WaitForProcessReductionsToComplete(pure_thread);

            MPI_Request req;
            int         ret;

            if (do_allreduce.has_value() && do_allreduce.value()) {
                sentinel("fixme blockinb");
                ret = MPI_Iallreduce(process_reduce_buf, recv_buf, count,
                                     datatype, op,
                                     thread_pure_comm->GetMpiComm(), &req);
            } else {
                ret = MPI_Reduce(process_reduce_buf, recv_buf, count, datatype,
                                 op, mpi_rank_for_root_in_pure_comm,
                                 thread_pure_comm->GetMpiComm());
            }

            assert(ret == MPI_SUCCESS);
            // MPI_Status status;
            // PureRT::do_mpi_wait_with_work_steal(pure_thread, &req, &status);
            WaitForNonLeadersToWrapup(pure_thread);
            ///////////////////////////
            ResetLeaderChannel(thread_num_in_process_in_comm);
            ResetChannelForReuse();
        } else {
            NonLeaderFinalization(thread_num_in_process_in_comm);
        }
    } else {
        // note: in the single process case, we still use process_reduce_buf
        // as the buffer of reduction and copy into the root's buffer so
        // threads who arrive before the root thread can get started
        // reducing. this also saves on code complexity, especially because
        // we can now leverage the fact that the reduce_process_buf is
        // aligned, which makes the chunking math easier.

        // single process
        // root should wait for all of the chunks to be reduced
        if (calling_pure_rank_in_comm == root_pure_rank_in_comm) {
            WaitForProcessReductionsToComplete(pure_thread);
            // then put data into buf on the root thread
            MEMCPY_IMPL(recv_buf, process_reduce_buf, size_bytes);
            WaitForNonLeadersToWrapup(pure_thread);
            ///////////////////////////
            ResetLeaderChannel(thread_num_in_process_in_comm);
            ResetChannelForReuse();
        } else {
            NonLeaderFinalization(thread_num_in_process_in_comm);
        }
    }

    // STEP 5: reset for next iteration
    // these are the thread-local variables
    ++thread_leader_ticket_num;
    thread_sense = !thread_sense;
}

void ReduceProcessChannel::InitializeBuffer() {
    switch (op) {
    case MPI_SUM: {
        MEMSET_IMPL(process_reduce_buf, 0, size_bytes);
        break;
    }
    case MPI_MAX: {
        assert(datatype == MPI_INT || datatype == MPI_DOUBLE);
        int* int_prb = reinterpret_cast<int*>(process_reduce_buf);
        if (datatype == MPI_INT) {
            std::fill_n(int_prb, count, INT_MIN);
        }
        if (datatype == MPI_DOUBLE) {
            std::fill_n(int_prb, count, DBL_MIN);
        }
        break;
    }
    case MPI_MIN: {
        assert(datatype == MPI_INT || datatype == MPI_DOUBLE);
        int* int_prb = reinterpret_cast<int*>(process_reduce_buf);
        if (datatype == MPI_INT) {
            std::fill_n(int_prb, count, INT_MAX);
        }
        if (datatype == MPI_DOUBLE) {
            std::fill_n(int_prb, count, DBL_MAX);
        }
        break;
    }
    // be careful -- for multiplication, base val has to be a 1 with the proper
    // datatype. Look at STL fill-style helpers.
    default: { sentinel("Unsupported operation %d", op); }
    }
}

// note: copied from bcast_process_channel
bool ReduceProcessChannel::ElectProcessLeader(
        int calling_pure_rank_in_comm, uint64_t thread_leader_ticket_num) {
    if (is_root_process) {
        return (calling_pure_rank_in_comm == root_pure_rank_in_comm);
    } else {
        if (thread_leader_ticket_num == process_leader_ticket_num.load()) {
            // no leader has been claimed yet -- try to claim it
            const bool is_leader =
                    process_leader_ticket_num.compare_exchange_strong(
                            thread_leader_ticket_num,
                            thread_leader_ticket_num + 1);
            return is_leader;
        }
    }
    return false;
}

void ReduceProcessChannel::WaitForProcessReductionsToComplete(
        PureThread* pure_thread) {

    while (num_threads_done.load(std::memory_order_acquire) <
           pure_ranks_this_comm_this_process) {
        MAYBE_WORK_STEAL(pure_thread);
    }
}

void ReduceProcessChannel::ResetLeaderChannel(
        int thread_num_in_process_in_comm) {
    auto chan = bound_chans[thread_num_in_process_in_comm];
    chan->ResetConsumption();
    chan->ClearBuf();
}

void ReduceProcessChannel::ResetChannelForReuse() {
    InitializeBuffer();
    assert(num_threads_done.load() == pure_ranks_this_comm_this_process);
    num_threads_done.store(0, std::memory_order_relaxed);
    num_non_leaders_totally_done.store(0, std::memory_order_relaxed);
    // fprintf(stderr, KGRN "   reset num threads to zero!!\n" KRESET);
    // simply invert the channel_ready_for_use variable
    const auto curr = channel_ready_for_use.load(std::memory_order_acquire);
    channel_ready_for_use.store(!curr, std::memory_order_release);
}

void ReduceProcessChannel::WrapupMyReduceChannel(
        int thread_num_in_process_in_comm) {
    bound_chans[thread_num_in_process_in_comm]->Wrapup(num_threads_computing);
}

void ReduceProcessChannel::NonLeaderFinalization(
        int thread_num_in_process_in_comm) {
    WrapupMyReduceChannel(thread_num_in_process_in_comm);
    ++num_non_leaders_totally_done;
}

void ReduceProcessChannel::MarkChunkDone() {
    // we may want to move this to use dropboxes if it's an issue
    ++num_threads_done; // the leader is watching this
}

void ReduceProcessChannel::WaitForNonLeadersToWrapup(PureThread* pure_thread) {
    const auto stopping = pure_ranks_this_comm_this_process - 1;
    while (num_non_leaders_totally_done.load(std::memory_order_acquire) <
           stopping) {
        MAYBE_WORK_STEAL(pure_thread);
    }
}
