// Author: James Psota
// File:   recv_flat_batcher_v1.cpp 

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

#include "pure/transport/experimental/v1/recv_flat_batcher_v1.h"
#include "mpi.h"
#include "pure/runtime/pure_process.h"
#include "pure/transport/experimental/v1/recv_flat_batch_channel_v1.h"
#include "pure/transport/recv_channel.h"

RecvFlatBatcher::RecvFlatBatcher(PureProcess* const pp, int mpi_size_arg,
                                 int my_mpi_rank, int num_threads_arg)
    : pure_process(pp), mpi_size(mpi_size_arg), mpi_rank(my_mpi_rank),
      num_threads(num_threads_arg) {

    posted_reqs = new LockedList*[num_threads];
    for (auto i = 0; i < num_threads; ++i) {
        posted_reqs[i] = new LockedList();
    }

    channels = new RecvFlatBatchChannel*[mpi_size];
    for (auto i = 0; i < mpi_size; ++i) {
        if (i != my_mpi_rank) {
            channels[i] = new RecvFlatBatchChannel(i);
        }
    }
}

RecvFlatBatcher::~RecvFlatBatcher() {
    for (auto i = 0; i < num_threads; ++i) {
        delete (posted_reqs[i]);
    }
    delete[](posted_reqs);

    for (auto i = 0; i < mpi_size; ++i) {
        if (i != mpi_rank) {
            delete (channels[i]);
        }
    }
    delete[](channels);
}

void RecvFlatBatcher::Finalize() const {}

/////////////////////////////////////////////////////////////////////////
// put a recv request into the posted_recvs queue. Later, check to see if the
// message has already arrived.
void RecvFlatBatcher::FindOrPostReceiveRequest(
        int thread_num, RecvChannel* const rc, void* dest_buf,
        int payload_bytes_max, int sender_pure_rank, int dest_pure_rank,
        MPI_Datatype dt, int tag) {

    const PostedRecvReq prr{rc, dest_buf, payload_bytes_max, sender_pure_rank,
                            dt, tag};
    LockedList* const   my_reqs = posted_reqs[thread_num];

    {
        // std::unique_lock<std::mutex> lock(my_reqs->lock);
        my_reqs->lock.lock();

        // 1. See if this message is already in the unexpected list of messages
        // for me
        PostedRecvReq needle{nullptr, nullptr, -1, sender_pure_rank, dt, tag};
        auto          unexpected_iter = std::find(my_reqs->unexpected.begin(),
                                         my_reqs->unexpected.end(), needle);

        if (unexpected_iter == my_reqs->unexpected.end()) {
            // not found, add to posted
            my_reqs->posted.insert(my_reqs->posted.end(), std::move(prr));
            if (BATCHER_DEBUG_PRINT)
                fprintf(stderr,
                        KYEL "[r%d] PostReceiveRequest: my posted list has %lu "
                             "reqeusts on it\n" KRESET,
                        dest_pure_rank, my_reqs->posted.size());
            my_reqs->lock.unlock();
        } else {
            // found, consume it
            void* const unexpected_buf   = unexpected_iter->dest_buf;
            int         unexpected_bytes = unexpected_iter->payload_bytes;
            assert(unexpected_iter->sender_pure_rank == sender_pure_rank);
            assert(unexpected_iter->datatype == dt);
            assert(unexpected_iter->tag == tag);
            my_reqs->unexpected.erase(unexpected_iter);
            // lock.unlock();
            my_reqs->lock.unlock();

            // copy into my desired buf
            assert(unexpected_bytes <= payload_bytes_max);
            memcpy(dest_buf, unexpected_buf, unexpected_bytes);
            free(unexpected_buf);

            // mark message as complete
            rc->MarkFlatBatcherMsgCompleted();
        }
    }
}

/////////////////////////////////////////////////////////////////////////
// Batcher Thread Functions

// The function that the batcher thread runs constantly (until it's killed)
void RecvFlatBatcher::BatcherThreadRunner() {
    char worker_name[16];
    sprintf(worker_name, "RecvF_Bat_%.3d", mpi_rank);
    pthread_set_name(worker_name);

#ifdef CORI_ARCH
    fprintf(stderr, KRED "HACK FOR EVALUATION -- REMOE THIS forcing send "
                         "bacher to run on a forced core\n" KRESET);
    set_cpu_affinity_on_linux(47);
#endif

    while (pure_process->RunRecvFlatBatcher()) {
        RecvAndProcessMessages();
    }
}

void RecvFlatBatcher::RecvAndProcessMessages() {
    // loop through all of the MPI endpoints
    for (auto m = 0; m < mpi_size; ++m) {
        if (m == mpi_rank) {
            continue;
        }

        void*      received_batch_buf;
        int        bytes_received;
        const auto did_recv = channels[m]->IsMPIRecvDone(&received_batch_buf,
                                                         &bytes_received);
        if (did_recv) {
            ProcessReceivedMessage(received_batch_buf, bytes_received);

            // kick off the next receive
            channels[m]->StartRecv();
        }
    }
}

void RecvFlatBatcher::ProcessReceivedMessage(void* received_batch_buf,
                                             int   total_bytes) {
    // Go through the batch buf, placing the payload (via copy) into the proper
    // place
    int   bytes_processed = 0;
    void* curr_buf        = received_batch_buf;

    while (bytes_processed < total_bytes) {
        // process header
        FlatBatcherHeaderSend* h =
                static_cast<FlatBatcherHeaderSend*>(curr_buf);
        h->Print();
        check(h->start_word == FLAT_BATCHER_HEADER_NONCE,
              "expected nonce (%d), but is %d", FLAT_BATCHER_HEADER_NONCE,
              h->start_word); // debug mode only

        // copy the payload into this dest buf
        void* msg_buf = static_cast<char*>(curr_buf) +
                        FlatBatcherHeader::SentSizeBytes();
        CopyOutMessage(h, msg_buf);

        bytes_processed +=
                FlatBatcherHeader::SentSizeBytes() + h->payload_bytes;

        // advance to the next message
        curr_buf = static_cast<char*>(msg_buf) + h->payload_bytes;
        assert(bytes_processed <= total_bytes);
    }
}

void RecvFlatBatcher::CopyOutMessage(FlatBatcherHeaderSend* const h,
                                     void* const                  msg_src_buf) {

    const int receiver_thread_num =
            pure_process->ThreadNumOnNodeFromPureRank(h->receiver_pure_rank);
    LockedList* thread_posted_reqs = posted_reqs[receiver_thread_num];

    const PostedRecvReq needle{nullptr,     nullptr, 0, h->sender_pure_rank,
                               h->datatype, h->tag};
    {
        // TODO: optimize locks
        // std::unique_lock<std::mutex> lock(thread_posted_reqs->lock);
        thread_posted_reqs->lock.lock();

        // first, try to find this message on the posted list
        const auto find_iter =
                std::find(thread_posted_reqs->posted.begin(),
                          thread_posted_reqs->posted.end(), needle);

        if (find_iter == thread_posted_reqs->posted.end()) {
            // didn't find it, so put it on the unexpected list
            void* const msg_dest_buf = malloc(h->payload_bytes);
            memcpy(msg_dest_buf, msg_src_buf, h->payload_bytes);
            PostedRecvReq new_unexpected{nullptr,          msg_dest_buf,
                                         h->payload_bytes, h->sender_pure_rank,
                                         h->datatype,      h->tag};
            thread_posted_reqs->unexpected.insert(
                    thread_posted_reqs->unexpected.end(),
                    std::move(new_unexpected));
            thread_posted_reqs->lock.unlock();
        } else {
            // found it -- put it where the user requested
            void* const        dest_buf = find_iter->dest_buf;
            RecvChannel* const found_rc = find_iter->rc;
            thread_posted_reqs->posted.erase(find_iter);
            // lock.unlock();
            thread_posted_reqs->lock.unlock();

            // copy the payload
            memcpy(dest_buf, msg_src_buf, h->payload_bytes);
            // mark that RC as done
            found_rc->MarkFlatBatcherMsgCompleted();
        }
    }
}