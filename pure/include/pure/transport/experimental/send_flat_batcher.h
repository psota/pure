// Author: James Psota
// File:   send_flat_batcher.h 

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

#ifndef SEND_FLAT_BATCHER
#define SEND_FLAT_BATCHER

#include "mpi.h"
#include "pure/common/pure_rt_enums.h"
#include "pure/support/typed_producer_consumer_queue.h"

#define FLAT_BATCHER_HEADER_NONCE 11111111

class SendFlatBatchChannel;
class PureProcess;

struct alignas(CACHE_LINE_BYTES) FlatBatcherHeader {
#if DEBUG_CHECK
    const int start_word = FLAT_BATCHER_HEADER_NONCE;
#endif
    const int          payload_bytes;
    const int          sender_pure_rank;
    const int          receiver_pure_rank;
    const MPI_Datatype datatype;
    const int          tag;
    // we are putting this at the end so the start of the memcpy is aligned.
    const void* payload_buf;

    // no padding should be needed as these are in an array in the
    // ProducerConsumerQueue

    FlatBatcherHeader(void* buf, int pb, int s, int r, int d, int t)
        : payload_buf(buf), payload_bytes(pb), sender_pure_rank(s),
          receiver_pure_rank(r), datatype(d), tag(t) {}

    static constexpr int SentSizeBytes() {
        // IMPORTANT!  keep this up to date
#if DEBUG_CHECK
        return 5 * sizeof(int) + sizeof(MPI_Datatype);
#else
        return 4 * sizeof(int) + sizeof(MPI_Datatype);
#endif
    }
};

// a special version for sending only -- no payload_buf
struct FlatBatcherHeaderSend {
#if DEBUG_CHECK
    const int start_word = FLAT_BATCHER_HEADER_NONCE;
#endif
    // MUST be kept in sync with the definition of FlatBatcherHeader
    const int          payload_bytes;
    const int          sender_pure_rank;
    const int          receiver_pure_rank;
    const MPI_Datatype datatype;
    const int          tag;

    void Print() {
#if DEBUG_CHECK
        assert(start_word == FLAT_BATCHER_HEADER_NONCE);
#endif
        if (BATCHER_DEBUG_PRINT)
            fprintf(stderr,
                    KGRN "   [ %dB  r%d -> r%d  d:%d  tag:%d ]\n" KRESET,
                    payload_bytes, sender_pure_rank, receiver_pure_rank,
                    static_cast<int>(datatype), tag);
    }
};

using SendQueue = ProducerConsumerQueue<FlatBatcherHeader>;

// one of these per PureProcess
class SendFlatBatcher {
  public:
    SendFlatBatcher(PureProcess*, int, int, int);
    ~SendFlatBatcher();
    void Finalize();

    void EnqueueMessage(int thread_num, void* msg_payload, int payload_bytes,
                        int sender_pure_rank, int receiver_pure_rank,
                        MPI_Datatype datatype, int tag);
    int  NumPendingEnqueues(int) const;
    void BatcherThreadRunner();

  private:
    SendQueue**            rank_send_queues;
    SendFlatBatchChannel** channels;
    PureProcess* const     pure_process;
    const int              mpi_size;
    const int              mpi_rank;
    const int              num_threads;
    //////////////
    uint64_t consume_req_q_empty     = 0;
    uint64_t consume_req_q_non_empty = 0;

    //////////////
    void ConsumeEnqueueRequests(int&);
};

#endif