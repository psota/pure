// Author: James Psota
// File:   rdma_mpi_channel.cpp 

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


#include "pure/transport/rdma_mpi_channel.h"
#include "pure/common/pure_pipeline.h"
#include "pure/support/helpers.h"
#include "pure/transport/mpi_comm_manager.h"
#include "pure/transport/recv_channel.h"
#include "pure/transport/send_channel.h"

/*
Later consider:
* timers
* assertions
x pure pipeline execution
*/

RdmaMPIChannel::RdmaMPIChannel(BundleChannelEndpoint* const bce,
                               const MPICommManager& comm_mgr, int my_mpi_rank,
                               int                  partner_mpi_rank,
                               PureRT::EndpointType endpoint_type,
                               PurePipeline&&       pp)
    : count_(bce->GetCount()), datatype_(bce->GetDatatype()),
      dequeue_pipeline_(std::move(pp)) {

    int ret = MPI_Alloc_mem(bce->NumPayloadBytes(), MPI_INFO_NULL, &buf_);
    assert(ret == MPI_SUCCESS);

    chan_comm_ = comm_mgr.Get(my_mpi_rank, partner_mpi_rank);
    MPI_Comm_group(chan_comm_, &chan_group_);

#if PURE_RDMA_MODE == PURE_RDMA_MODE_FENCE
    sentinel("Use of PURE_RDMA_MODE_FENCE mode is currently not supported.");
#endif

    int group_sz;
#if DEBUG_CHECK
    int comm_sz;
    MPI_Comm_size(chan_comm_, &comm_sz);
    assert(comm_sz == 2);
    MPI_Group_size(chan_group_, &group_sz);
    assert(group_sz == 2);
#endif

    // create the group for the PSCW calls (enqueue and dequeue)
    int chan_comm_rank;
    MPI_Comm_rank(chan_comm_, &chan_comm_rank);
    assert(chan_comm_rank == 0 || chan_comm_rank == 1);
    partner_chan_comm_rank_ = 1 - chan_comm_rank;
    MPI_Group_incl(chan_group_, 1, &partner_chan_comm_rank_,
                   &origin_or_target_group_);

    MPI_Group_size(origin_or_target_group_, &group_sz);
    assert(group_sz == 1);

    if (endpoint_type == PureRT::EndpointType::SENDER) {
        bce->SetSendChannelBuf(buf_);
        // we only support PUTs for now
        MPI_Win_create(nullptr, 0, bce->GetDatatypeBytes(), MPI_INFO_NULL,
                       chan_comm_, &win_);
        dynamic_cast<SendChannel*>(bce)->SetInitializedState();
    } else {
        // receiver
        bce->SetRecvChannelBuf(buf_);
        MPI_Win_create(buf_, count_, bce->GetDatatypeBytes(), MPI_INFO_NULL,
                       chan_comm_, &win_);
        dynamic_cast<RecvChannel*>(bce)->SetInitializedState();
    }
}

RdmaMPIChannel::~RdmaMPIChannel() {
    MPI_Free_mem(buf_);
    MPI_Group_free(&chan_group_);
    MPI_Group_free(&origin_or_target_group_);
    MPI_Win_free(&win_);
}

////////////////////////////////////////////////
void RdmaMPIChannel::Enqueue(const buffer_t sender_buf,
                             const size_t   count_to_send) {
    // for now, just support runtime buffer and full window size
    assert(sender_buf == nullptr);
    assert(count_to_send == count_);
    int ret;
#if PURE_RDMA_MODE == PURE_RDMA_MODE_PSCW
    ret = MPI_Win_start(origin_or_target_group_, 0, win_);
#elif PURE_RDMA_MODE == PURE_RDMA_MODE_LOCK

#endif
    assert(ret == MPI_SUCCESS);
    const int target_displacement = 0;
    ret = MPI_Put(buf_, count_, datatype_, partner_chan_comm_rank_,
                  target_displacement, count_, datatype_, win_);
    assert(ret == MPI_SUCCESS);
}

void RdmaMPIChannel::EnqueueWait() {
    int ret;
#if PURE_RDMA_MODE == PURE_RDMA_MODE_PSCW
    ret = MPI_Win_complete(win_);
#elif PURE_RDMA_MODE == PURE_RDMA_MODE_LOCK

#endif
    assert(ret == MPI_SUCCESS);
}

////////////////////////////////////////////////
void RdmaMPIChannel::Dequeue(buffer_t user_buf) {
    assert(user_buf == nullptr); // not currently supported
    int ret;
#if PURE_RDMA_MODE == PURE_RDMA_MODE_PSCW
    ret = MPI_Win_post(origin_or_target_group_,
                       MPI_MODE_NOSTORE | MPI_MODE_NOPUT, win_);
#elif PURE_RDMA_MODE == PURE_RDMA_MODE_LOCK

#endif
    assert(ret == MPI_SUCCESS);
}

const PureContRetArr& RdmaMPIChannel::DequeueWait() {
    int ret;
#if PURE_RDMA_MODE == PURE_RDMA_MODE_PSCW
    ret = MPI_Win_wait(win_);
#elif PURE_RDMA_MODE == PURE_RDMA_MODE_LOCK

#endif
    assert(ret == MPI_SUCCESS);

#if PCV4_OPTION_EXECUTE_CONTINUATION
    if (dequeue_pipeline_.Empty()) {
        return static_empty_pure_cont_ret_arr;
    } else {
        assert(dequeue_pipeline_.NumStages() > 0);
        // TODO: add support for user buf and user counts
        dequeue_pipeline_.ExecutePCPipeline(buf_, count_);
        return dequeue_pipeline_.GetReturnVals();
    }
#else
    // returns a reference to a const empty vector
    return static_empty_pure_cont_ret_arr;
#endif // no continuation
}
