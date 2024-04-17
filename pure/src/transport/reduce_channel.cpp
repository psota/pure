// Author: James Psota
// File:   reduce_channel.cpp 

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

#include "pure/transport/bundle_channel_endpoint.h"
#include "pure/transport/reduce_process_channel.h"
#include <atomic>
#include <optional>

ReduceChannel::ReduceChannel(int count_arg, const MPI_Datatype& datatype_arg,
                             int                    sender_pure_rank_in_comm,
                             int                    root_pure_rank_in_comm_arg,
                             channel_endpoint_tag_t channel_endpoint_tag_arg,
                             MPI_Op mpi_op_arg, PureThread* pure_thread_arg,
                             PureComm const* const thread_pure_comm_arg)
    : BundleChannelEndpoint(nullptr, nullptr, count_arg, datatype_arg,
                            sender_pure_rank_in_comm, PureRT::PURE_UNUSED_TAG,
                            root_pure_rank_in_comm_arg,
                            channel_endpoint_tag_arg, pure_thread_arg),
      mpi_op(mpi_op_arg), pure_rank_in_comm(sender_pure_rank_in_comm),
      thread_pure_comm(thread_pure_comm_arg) {}

void ReduceChannel::WaitForInitialization() const {
    reduce_process_channel->WaitForInitialization(pure_thread);
}

void ReduceChannel::Reduce(buffer_t src_buf, buffer_t recv_buf,
                           bool*               is_process_leader,
                           std::optional<bool> do_allreduce) {

    check(buf.load() == nullptr,
          "In ReduceChannel::Reduce, src_buf should be not set (i.e., nullptr) "
          "but it is %p",
          buf.load());
    StoreBuf(src_buf);
    reduce_process_channel->ReduceBlocking(
            pure_thread, thread_pure_comm, src_buf, recv_buf, leader_ticket_num,
            thread_sense, is_process_leader, do_allreduce);
}

void ReduceChannel::SetReduceProcessChannel(ReduceProcessChannel* const rpc) {
    assert(reduce_process_channel == nullptr);
    reduce_process_channel = rpc;
}

void* ReduceChannel::GetBuf() const {
    return buf.load(std::memory_order_acquire);
}

void ReduceChannel::ClearBuf() { StoreBuf(nullptr); }

void ReduceChannel::StoreBuf(buffer_t buf_arg) {
    return buf.store(buf_arg, std::memory_order_release);
}

void ReduceChannel::MarkDoneConsumption() { ++num_consumed; }

void ReduceChannel::Wrapup(int num_threads_computing) {
    WaitForAllConsumption(num_threads_computing);
    ClearBuf();
    ResetConsumption();
}

void ReduceChannel::WaitForAllConsumption(int num_threads_computing) {
    auto pt = GetPureThread();
    // MarkDoneConsumption isn't called on my own chunk as we use memcpy to
    // initialize the buffer
    while (num_consumed.load(std::memory_order_acquire) <
           num_threads_computing) {
        MAYBE_WORK_STEAL(pt);
    }
}

void ReduceChannel::ResetConsumption() {
    num_consumed.store(0, std::memory_order_release);
}
