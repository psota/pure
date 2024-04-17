// Author: James Psota
// File:   recv_flat_batch_channel_v2.cpp 

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

#include "pure/transport/experimental/v2/recv_flat_batch_channel_v2.h"
#include "mpi.h"
#include "pure/support/helpers.h"
#include "pure/transport/experimental/send_flat_batch_channel.h"
#include <queue>
#include <vector>

RecvFlatBatchChannel::RecvFlatBatchChannel(int mate_mpi_rank_arg) {
    // create mpi structs and kick off their recvs
    for (auto i = 0; i < max_in_flight_sends; ++i) {
        auto mpi_struct = new RecvFlatBatchChannelMpi(mate_mpi_rank_arg);
        mpi_struct->DoMPIIrecv();
        receiving_mpi_structs.push(mpi_struct);
    }

    being_consumed_mpi_structs.reserve(max_in_flight_sends);
    AssertMpiStructInvariants();
}

RecvFlatBatchChannel::~RecvFlatBatchChannel() {
    AssertMpiStructInvariants();

    while (!receiving_mpi_structs.empty()) {
        RecvFlatBatchChannelMpi* const s = receiving_mpi_structs.front();
        receiving_mpi_structs.pop();
        delete (s);
    }

    std::for_each(being_consumed_mpi_structs.begin(),
                  being_consumed_mpi_structs.end(),
                  [](RecvFlatBatchChannelMpi* s) { delete (s); });
}

void RecvFlatBatchChannel::Finalize() {
    // this is executed by the last recv rank thread to finish work. The worker
    // thread IS STILL RUNNING at this point.

    // right now nothing needs to be done.
    // also, this code below is problematic -- as the worker thread is still
    // running and could be in the moving things from one of the following data
    // structures to another. They are not thread safe.

    // for (auto i = 0; i < receiving_mpi_structs.size(); ++i) {
    //     RecvFlatBatchChannelMpi* const s = receiving_mpi_structs.front();
    //     receiving_mpi_structs.pop();
    //     s->Finalize();
    //     receiving_mpi_structs.push(s); // for later destruction
    // }

    // std::for_each(being_consumed_mpi_structs.begin(),
    //               being_consumed_mpi_structs.end(),
    //               [](RecvFlatBatchChannelMpi* s) { s->Finalize(); });
}

// note: this maybe should be gauranteed to succeed and use an MPI_Wait
void RecvFlatBatchChannel::TryGetMsgBatchBuf(
        void** batch_buf_out, int* payload_bytes_out,
        RecvFlatBatchChannelMpi** curr_mpi_out) {

    // approach: for the current mpi struct, return the buffer for consumption
    // by receiver ranks iff it's actually in a consumed state
    RecvFlatBatchChannelMpi* const curr_mpi = GetCurrReceivingMpiStruct();
    if (curr_mpi != nullptr && curr_mpi->IsRecvComplete()) {
        *batch_buf_out     = curr_mpi->batch_buf;
        *payload_bytes_out = curr_mpi->received_bytes;
        *curr_mpi_out      = curr_mpi;
    } else {
        *batch_buf_out     = nullptr;
        *payload_bytes_out = 0;
        *curr_mpi_out      = nullptr;
    }
}

// WARNING: only the worker thread can call this.
void RecvFlatBatchChannel::AssertMpiStructInvariants() const {
#if DEBUG_CHECK
    const auto num_receiving      = receiving_mpi_structs.size();
    const auto num_being_consumed = being_consumed_mpi_structs.size();

    if (num_receiving + num_being_consumed != max_in_flight_sends) {
        fprintf(stderr,
                "num_receiving mpi_structs: %d  num_being_consumed: %d   "
                "max_in_flight_sends: %d\n",
                num_receiving, num_being_consumed, max_in_flight_sends);
        PureRT::print_pure_backtrace(stderr, '\n');
        sentinel("aborting due to mismatch");
    }
#endif
}

// may return null
RecvFlatBatchChannelMpi* const
RecvFlatBatchChannel::GetCurrReceivingMpiStruct() {
    return receiving_mpi_structs.front();
}

// MPI struct management functions
void RecvFlatBatchChannel::MoveReceivedMpiStructToBeingConsumed() {
    AssertMpiStructInvariants();
    RecvFlatBatchChannelMpi* received_struct = receiving_mpi_structs.front();
    receiving_mpi_structs.pop();
    being_consumed_mpi_structs.push_back(received_struct);
    AssertMpiStructInvariants();
}

void RecvFlatBatchChannel::MigrateConsumedMessagesToBeingReceived() {
    // go through all mpi structs. If they are fully consumed, move them back to
    // the receiving queue, and initiate a new receive.
    AssertMpiStructInvariants();
    // TODO: convert this to use an iterator?
    for (auto i = 0; i < being_consumed_mpi_structs.size(); ++i) {
        RecvFlatBatchChannelMpi* this_struct = being_consumed_mpi_structs[i];
        if (this_struct->AllMsgsConsumed()) {
            this_struct->DoMPIIrecv();
            receiving_mpi_structs.push(this_struct);
            being_consumed_mpi_structs.erase(
                    being_consumed_mpi_structs.begin() + i);
        }
    }
    AssertMpiStructInvariants();
}