// Author: James Psota
// File:   recv_flat_batch_channel_v2.h 

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

#ifndef RECV_FLAT_BATCH_CHANNEL_V2_H
#define RECV_FLAT_BATCH_CHANNEL_V2_H

#include "mpi.h"
#include "pure/common/pure_rt_enums.h"
#include "pure/support/helpers.h"
#include "pure/transport/experimental/send_flat_batch_channel.h"
#include <atomic>
#include <queue>
#include <vector>

using std::atomic;
using std::queue;
using std::vector;

struct RecvFlatBatchChannelMpi {

    enum class RecvState {
        none,               // 0
        msg_incoming,       // 1
        msg_being_consumed, // 2
    };

    /////////////////////
    void*       batch_buf;
    MPI_Request recv_req = 0;
    const int   mate_mpi_rank;
    const int   batch_tag = PureRT::PURE_DIRECT_CHAN_BATCHER_TAG; // special tag
    int         size_bytes_max;
    int         received_bytes;
    RecvState   state = RecvState::none;

    alignas(CACHE_LINE_BYTES) atomic<int> unconsumed_msgs = 0;

    /////////////////////////
    explicit RecvFlatBatchChannelMpi(int mate_mpi_rank_arg)
        : mate_mpi_rank(mate_mpi_rank_arg) {
        batch_buf = jp_memory_alloc<1, CACHE_LINE_BYTES, 1>(
                send_bytes_buf_init_sz, &size_bytes_max);
    }

    ~RecvFlatBatchChannelMpi() {
        // WARNING: AllMsgsConsumed has a side-effect of changing the state to
        // none!!!
        check_always(AllMsgsConsumed(), "Expected all messages to be consumed "
                                        "upon destruction on this MPI struct.");

        // cancel the MPI request and free it
        // We know the state is none given that AllMsgsConsumed was called and
        // that changes the state to none if they are indeed done.
        if (DoingNothing()) {
            const auto ret = MPI_Cancel(&recv_req);
            assert(ret == MPI_SUCCESS);
            const auto ret2 = MPI_Request_free(&recv_req);
            assert(ret2 == MPI_SUCCESS);
        }
        free(batch_buf);
    }

    void Finalize() { // do nothing as of now
    }

    bool DoingNothing() const { return (state == RecvState::none); }
    bool WaitingForMsg() const { return (state == RecvState::msg_incoming); }
    bool WaitingForConsumption() const {
        return (state == RecvState::msg_being_consumed);
    }

    int GetSizeBytesMax() const { return size_bytes_max; }

    void DoMPIIrecv() {
        // the caller is responsible for calling this when it is ok to do so
        assert(DoingNothing());
        MPI_Status status;
        const auto ret =
                MPI_Irecv(batch_buf, size_bytes_max, MPI_BYTE, mate_mpi_rank,
                          batch_tag, MPI_COMM_WORLD, &recv_req);
        assert(ret == MPI_SUCCESS);
        state = RecvState::msg_incoming;
    }

    bool IsRecvComplete() {
        bool recv_complete = false;
        if (WaitingForMsg()) {
            // see if it's actually done by checking the MPI runtime
            int        done;
            MPI_Status status;
            const auto ret = MPI_Test(&recv_req, &done, &status);
            assert(ret == MPI_SUCCESS);

            const auto count_ret =
                    MPI_Get_count(&status, MPI_BYTE, &received_bytes);
            assert(count_ret == MPI_SUCCESS);
            check(received_bytes <= size_bytes_max,
                  "received_bytes=%d but size_bytes_max=%d", received_bytes,
                  size_bytes_max);

            if (done == 1) {
                state         = RecvState::msg_being_consumed;
                recv_complete = true;
            }
        }
        return recv_complete;
    }

    // called by the recv rank threads
    void MarkMsgConsumed() {
        // note: the counter may go negative temporarily while the worker thread
        // is processing the message batch. That's ok -- the worker knows when
        // it's done and won't look at unconsumed_msgs until it's ready to do
        // so.
        unconsumed_msgs.fetch_sub(1);
    }

    // called by the worker thread
    void AddNumMsgsInBatch(int n) {
        unconsumed_msgs.fetch_add(n);
        assert(unconsumed_msgs.load() >= 0);
    }

    // called by the worker thread
    bool AllMsgsConsumed() {
        const auto consumed = (unconsumed_msgs.load() == 0);
        if (consumed) {
            state = RecvState::none;
        }

        // note: now it's safe to kick off the next Irecv. Only the worker
        // thread should do so but it should be done.
        return consumed;
    }
};

///////////////////////////////////////////////////////
///////////////////////////////////////////////////////
class RecvFlatBatchChannel {
  public:
    explicit RecvFlatBatchChannel(int mate_mpi_rank_arg);
    ~RecvFlatBatchChannel();
    void Finalize();
    void TryGetMsgBatchBuf(void** batch_buf_out, int* payload_bytes_out,
                           RecvFlatBatchChannelMpi** curr_mpi_out);
    void MoveReceivedMpiStructToBeingConsumed();
    void MigrateConsumedMessagesToBeingReceived();

  private:
    queue<RecvFlatBatchChannelMpi*>  receiving_mpi_structs;
    vector<RecvFlatBatchChannelMpi*> being_consumed_mpi_structs;

    ////////////////
    void                           AssertMpiStructInvariants() const;
    RecvFlatBatchChannelMpi* const GetCurrReceivingMpiStruct();
};

#endif