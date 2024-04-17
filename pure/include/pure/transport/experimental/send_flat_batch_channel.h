// Author: James Psota
// File:   send_flat_batch_channel.h 

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

#ifndef SEND_FLAT_BATCH_CHANNEL
#define SEND_FLAT_BATCH_CHANNEL

#include "mpi.h"
#include "pure/common/pure_rt_enums.h"
#include "pure/support/benchmark_timer.h"
#include "pure/support/helpers.h"
#include <cstddef>
#include <vector>

using std::vector;

// consider lowering this -- maybe better to wait a bit?
#define FLAT_BATCHER_MAX_IN_FLIGHT_MPI_SENDS 32
#define FLAT_BATCHER_DO_NONBLOCKING_SEND_TEST 0

struct FlatBatcherHeader;

/* there's one of these for each mate MPI rank recipient and it's driven solely
 * by the batcher helper thread (not the individual ranks).
 */

class SendFlatBatcher;

// TODO: make this at least the MTU for cori MPI
static const size_t send_bytes_buf_init_sz = FLAT_BATCHER_SEND_BYTES_THRESHOLD;
static const auto   send_bytes_threshold_factor = 0.8f;
static const size_t send_queue_size             = 1024;

static const size_t max_in_flight_sends = FLAT_BATCHER_MAX_IN_FLIGHT_MPI_SENDS;

struct SendFlatBatchChannelMpi {
    void*       batch_buf;
    MPI_Request send_req = 0;
    const int   mate_mpi_rank;
    const int   batch_tag = PureRT::PURE_DIRECT_CHAN_BATCHER_TAG; // special tag
    int         size_bytes_max;
    bool        msg_pending = false;

    explicit SendFlatBatchChannelMpi(int mate_mpi_rank_arg)
        : mate_mpi_rank(mate_mpi_rank_arg) {
        batch_buf = jp_memory_alloc<1, CACHE_LINE_BYTES, 1>(
                send_bytes_buf_init_sz, &size_bytes_max);
    }

    ~SendFlatBatchChannelMpi() { free(batch_buf); }

    void Finalize() {
        if (msg_pending) {
            const auto ret = MPI_Wait(&send_req, MPI_STATUS_IGNORE);
            assert(ret == MPI_SUCCESS);
        }
    }

    int GetSizeBytesMax() const { return size_bytes_max; }

    void DoMPIIsend(int bytes_to_send) {

        assert(msg_pending == false);
        assert(bytes_to_send <= size_bytes_max);
        const auto ret =
                MPI_Isend(batch_buf, bytes_to_send, MPI_BYTE, mate_mpi_rank,
                          batch_tag, MPI_COMM_WORLD, &send_req);
        assert(ret == MPI_SUCCESS);
        msg_pending = true;
    }

    bool IsSendPending() {
        // fprintf(stderr, "Doing mpi test on req %d (%p)\n", send_req,
        // &send_req);
        if (msg_pending) {
            // see if it's actually done
            int        done;
            const auto ret = MPI_Test(&send_req, &done, MPI_STATUS_IGNORE);
            assert(ret == MPI_SUCCESS);

            if (done == 1) {
                msg_pending = false;
            }
        }
        return msg_pending;
    }

    void WaitOnSendCompletion() {
        if (msg_pending) {
            const auto ret = MPI_Wait(&send_req, MPI_STATUS_IGNORE);
            assert(ret == MPI_SUCCESS);
            msg_pending = false;
        }
    }
};

class alignas(CACHE_LINE_BYTES) SendFlatBatchChannel {

  public:
    explicit SendFlatBatchChannel(int);
    ~SendFlatBatchChannel();
    void        Finalize();
    void        ConsumeEnqueueRequests();
    void        AddMessage(FlatBatcherHeader* const h);
    static void PrintBatchedMessage(void const* const ptr, void* batch_buf,
                                    int total_bytes, bool is_sender);

  private:
    vector<SendFlatBatchChannelMpi*> mpi_structs;
    int                              size_bytes_max;
    int                              size_bytes_send_threshold;

    // bytes_added_cume is for the curr_mpi_struct only
    int            bytes_added_cume = 0;
    int            curr_mpi_struct  = 0;
    BenchmarkTimer send_pending_check, do_isend, memcpy_timer;

    /////////////////////
    void                     AddMsgToBatch(FlatBatcherHeader* const h);
    void                     SendAndReset();
    void                     Reset();
    void                     DoMPISend();
    int                      BytesAvailable() const;
    SendFlatBatchChannelMpi* GetCurrMPIStruct();
    void                     ConcludeMPIISend();
};

#endif
