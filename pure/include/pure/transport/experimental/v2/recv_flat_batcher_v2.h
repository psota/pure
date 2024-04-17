// Author: James Psota
// File:   recv_flat_batcher_v2.h 

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

#ifndef RECV_FLAT_BATCHER_V2_H
#define RECV_FLAT_BATCHER_V2_H

#include "mpi.h"
#include "pure/support/typed_producer_consumer_queue.h"
#include "pure/transport/experimental/send_flat_batcher.h"
#include <list>

#define FLAT_BATCHER_INIT_RECV_RANK_LISTS_SZ 16
#define FLAT_BATCHER_RECV_WORKER_WAIT_ON_RECV_QUEUE_WRITE 1

class RecvFlatBatchChannel;
class RecvFlatBatchChannelMpi;
class PureProcess;
class RecvChannel;

// align to cache line to prevent false-sharing inside the ProducerConsumerQueue
// TODO: convert this to a templatized struct:
// https://stackoverflow.com/questions/64972740/c-class-specifier-alignas-option-via-template
// then just us using to keep the same class names.
struct alignas(CACHE_LINE_BYTES) AlignedRecvInstructions {
    RecvFlatBatchChannelMpi* mpi_struct{nullptr};
    void*                    msg_buf{nullptr};
    int                      payload_bytes{0};
    int                      sender_pure_rank{0};
#if DEBUG_CHECK
    int receiver_pure_rank{0};
#endif
    MPI_Datatype datatype{0};
    int          tag{0};

    AlignedRecvInstructions() : mpi_struct(nullptr), msg_buf(nullptr) {}

    AlignedRecvInstructions(RecvFlatBatchChannelMpi* const mpi_struct_arg,
                            void* const                    msg_buf_arg,
                            const int                      payload_bytes_arg,
                            const int                      sender_pure_rank_arg,
#if DEBUG_CHECK
                            const int receiver_pure_rank_arg,
#endif
                            const MPI_Datatype datatype_arg, const int tag_arg)
        : mpi_struct(mpi_struct_arg), msg_buf(msg_buf_arg),
          payload_bytes(payload_bytes_arg),
          sender_pure_rank(sender_pure_rank_arg),
#if DEBUG_CHECK
          receiver_pure_rank(receiver_pure_rank_arg),
#endif
          datatype(datatype_arg), tag(tag_arg) {
    }

    bool Matches(int sender_pure_rank, MPI_Datatype dt, int tag) {
        return (this->sender_pure_rank == sender_pure_rank &&
                this->tag == tag && this->datatype == dt);
    }
};

// UnexpectedList type
struct RecvInstructions {
    // IMPORTANT: keep in sync with above
    // same contents but not aligned as it's all within one thread
    RecvFlatBatchChannelMpi* mpi_struct{nullptr};
    void*                    msg_buf{nullptr};
    int                      payload_bytes{0};
    int                      sender_pure_rank{0};
#if DEBUG_CHECK
    int receiver_pure_rank{0};
#endif
    MPI_Datatype datatype{0};
    int          tag{0};

    // copy constructor from AlignedRecvInstructions
    explicit RecvInstructions(const AlignedRecvInstructions& ari)
        : mpi_struct(ari.mpi_struct), msg_buf(ari.msg_buf),
          payload_bytes(ari.payload_bytes),
          sender_pure_rank(ari.sender_pure_rank),
#if DEBUG_CHECK
          receiver_pure_rank(ari.receiver_pure_rank),
#endif
          datatype(ari.datatype), tag(ari.tag) {
    }

    RecvInstructions(int dt, int t)
        : mpi_struct(nullptr), msg_buf(nullptr), datatype(dt), tag(t) {}

    bool operator==(const RecvInstructions& rhs) {
        return this->tag == rhs.tag && this->datatype == rhs.datatype;
    }
};

// PendingList type
struct RecvReq {
    // RecvChannel* const rc;
    RecvChannel* rc{nullptr};
    void*        dest_buf{nullptr};
    int          payload_bytes{0};
    MPI_Datatype datatype{0};
    int          tag{0};

    //////////////
    RecvReq(RecvChannel* rc_arg, void* const dest_buf_arg,
            const int payload_bytes_arg, const MPI_Datatype datatype_arg,
            const int tag_arg)
        : rc(rc_arg), dest_buf(dest_buf_arg), payload_bytes(payload_bytes_arg),
          datatype(datatype_arg), tag(tag_arg) {}

    RecvReq(int dt, int t)
        : rc(nullptr), dest_buf(nullptr), payload_bytes(-1), datatype(dt),
          tag(t) {}

    // clang-tidy:  overloading 'operator==' is disallowed
    // [fuchsia-overloaded-operator] but not sure why?
    bool operator==(const RecvReq& rhs) {
        return this->tag == rhs.tag && this->datatype == rhs.datatype;
    }
};

// TODO: compare vector to list usage. Also compare a single vector to a
// sender-thread-num-indexed array of vectors (or lists).
// WARNING: this should probably be reduced later or spinning should be
// introduced
static const size_t recv_queue_size = 4096;
using PendingList                   = std::vector<RecvReq>;
using UnexpectedList                = std::vector<RecvInstructions>;
using RecvQueue = ProducerConsumerQueue<AlignedRecvInstructions>;

// There is one of these per PureProcess
class RecvFlatBatcher {

  public:
    RecvFlatBatcher(PureProcess* const, int, int, int);
    ~RecvFlatBatcher();
    void Finalize();
    //////////////////
    void PostReceiveRequest(int thread_num, RecvChannel* const rc,
                            void* dest_buf, int payload_bytes_max,
                            int sender_pure_rank, int dest_pure_rank,
                            MPI_Datatype dt, int tag);
    void WaitRecvRequest(int thread_num, RecvChannel* const rc, void* dest_buf,
                         int payload_bytes_max, int sender_pure_rank,
                         int dest_pure_rank, MPI_Datatype dt, int tag);
    //////////////////
    void BatcherThreadRunner();

  private:
    RecvFlatBatchChannel** channels; // for each mate mpi rank
    // for each dest thread num for each sender thread num
    RecvQueue** recv_queues;

    PureProcess* pure_process = nullptr;

#if FLAT_BATCHER_SEND_BYTES_THRESHOLD == 0
    void* recv_buf;
    int   payload_bytes_max = 65'536;
#endif

    const int mpi_size;
    const int mpi_rank;
    const int num_threads;

    // perf counters for thread 0 only
    alignas(CACHE_LINE_BYTES) uint64_t wait_read_my_queue_try_t0 = 0;
    uint64_t wait_read_my_queue_success_t0                       = 0;

    // one list per sender thread num
    // so, posted_lists[5] is the posted list for messagess from thread num
    // 5
    alignas(CACHE_LINE_BYTES) PendingList*** posted_lists;
    UnexpectedList*** unexpected_lists;
    //////////////////
    PendingList* const    GetPendingRecvList(int sender_pure_rank,
                                             int dest_thread_num) const;
    UnexpectedList* const GetUnexpectedRecvList(int sender_pure_rank,
                                                int dest_thread_num) const;
    void ConsumeAndCompleteMsg(void* const msg_buf, int inst_payload_bytes,
                               RecvFlatBatchChannelMpi* mpi_struct,
                               void* const dest_buf, int payload_bytes_max,
                               RecvChannel* const rc);
    void RecvAndProcessBatchMessages();
    void RecvAndProcessDirectMessages();
    void PlaceBatchBufMsgsIntoRecvQueues(
            void* received_batch_buf, int total_bytes,
            RecvFlatBatchChannelMpi* const this_mpi_struct);
};

#endif
