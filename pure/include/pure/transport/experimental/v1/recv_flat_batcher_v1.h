// Author: James Psota
// File:   recv_flat_batcher_v1.h 

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

#ifndef RECV_FLAT_BATCHER_V1_H
#define RECV_FLAT_BATCHER_V1_H

#include "mpi.h"
#include "pure/transport/experimental/send_flat_batcher.h"
#include <folly/SpinLock.h>
#include <list>

class RecvFlatBatchChannel;
class PureProcess;
class RecvChannel;

struct PostedRecvReq {
    RecvChannel*       rc;
    void*              dest_buf;
    const int          payload_bytes;
    const int          sender_pure_rank;
    const MPI_Datatype datatype;
    const int          tag;

    //////////////
    bool operator==(const PostedRecvReq& rhs) {
        return this->sender_pure_rank == rhs.sender_pure_rank &&
               this->tag == rhs.tag && this->datatype == rhs.datatype;
    }
};

static_assert(sizeof(folly::SpinLock) <= sizeof(std::list<PostedRecvReq>));

struct alignas(CACHE_LINE_BYTES) LockedList {
    std::list<PostedRecvReq> unexpected;
    std::list<PostedRecvReq> posted;
    folly::SpinLock          lock;
};

// There is one of these per PureProcess
class RecvFlatBatcher {

  public:
    RecvFlatBatcher(PureProcess* const pp, int mpi_sz, int mpi_rank,
                    int num_threads);
    ~RecvFlatBatcher();
    void Finalize() const;
    void FindOrPostReceiveRequest(int thread_num, RecvChannel* const rc,
                                  void* dest_buf, int payload_bytes_max,
                                  int sender_pure_rank, int dest_pure_rank,
                                  MPI_Datatype dt, int tag);
    void BatcherThreadRunner();

  private:
    LockedList**           posted_reqs;
    RecvFlatBatchChannel** channels;
    PureProcess*           pure_process = nullptr;
    const int              mpi_size;
    const int              mpi_rank;
    const int              num_threads;
    //////////////////
    void RecvAndProcessMessages();
    void ProcessReceivedMessage(void* received_batch_buf, int total_bytes);
    void CopyOutMessage(FlatBatcherHeaderSend* const h,
                        void* const                  msg_src_buf);
};

#endif