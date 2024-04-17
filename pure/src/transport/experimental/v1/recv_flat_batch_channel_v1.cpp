// Author: James Psota
// File:   recv_flat_batch_channel_v1.cpp 

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

#include "pure/transport/experimental/v1/recv_flat_batch_channel_v1.h"
#include "mpi.h"
#include "pure/support/helpers.h"
#include "pure/transport/experimental/send_flat_batch_channel.h"

RecvFlatBatchChannel::RecvFlatBatchChannel(int mate_mpi_rank_arg)
    : mate_mpi_rank(mate_mpi_rank_arg) {

    batch_buf = jp_memory_alloc<1, CACHE_LINE_BYTES, 1>(send_bytes_buf_init_sz,
                                                        &size_bytes_max);
}

RecvFlatBatchChannel::~RecvFlatBatchChannel() {
    free(batch_buf);
    if (irecv_outstanding) {
        const auto ret = MPI_Cancel(&recv_req);
        assert(ret == MPI_SUCCESS);

        const auto ret2 = MPI_Request_free(&recv_req);
        assert(ret2 == MPI_SUCCESS);
    }
}

bool RecvFlatBatchChannel::IsMPIRecvDone(void** batch_buf_out,
                                         int*   payload_bytes_out) {

    if (irecv_outstanding == false) {
        StartRecv();
    }

    // now, see if the request is ready
    MPI_Status status;
    int        completed;
    const auto ret = MPI_Test(&recv_req, &completed, &status);
    assert(ret == MPI_SUCCESS);

    if (completed) {
        irecv_outstanding = false;

        const int ret = MPI_Get_count(&status, MPI_BYTE, &received_bytes);
        assert(ret == MPI_SUCCESS);
        assert(received_bytes <= size_bytes_max);

        // set the output params
        *batch_buf_out     = batch_buf;
        *payload_bytes_out = received_bytes;

        // we can't kick off the next irecv until we've consumed the buffer
        // completely
    } else {
        assert(irecv_outstanding == true);
    }

    return completed;
}

void RecvFlatBatchChannel::StartRecv() {
    assert(irecv_outstanding == false);
    const auto ret =
            MPI_Irecv(batch_buf, size_bytes_max, MPI_BYTE, mate_mpi_rank,
                      batch_tag, MPI_COMM_WORLD, &recv_req);
    assert(ret == MPI_SUCCESS);
    irecv_outstanding = true;
}