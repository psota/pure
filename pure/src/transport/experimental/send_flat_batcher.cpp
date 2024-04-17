// Author: James Psota
// File:   send_flat_batcher.cpp 

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

#include "pure/transport/experimental/send_flat_batcher.h"
#include "pure/runtime/pure_process.h"
#include "pure/support/helpers.h"
#include "pure/transport/experimental/send_flat_batch_channel.h"

/* stuff to do
 * thread this through and create this object in pure process
 * use in send chan
 * use in recv chan
 * pick up where I left off with thread is draining buffers
 * opt -- consider popping many at once at the end of enquueeleader (for each
 * thread)
 * have two or more mpi sends/waits in flight at any one time. batch while the
 * send is happening.
 */

SendFlatBatcher::SendFlatBatcher(PureProcess* pure_process_arg,
                                 int mpi_size_arg, int my_mpi_rank,
                                 int num_threads_arg)
    : pure_process(pure_process_arg), mpi_size(mpi_size_arg),
      mpi_rank(my_mpi_rank), num_threads(num_threads_arg) {

#if FLAT_BATCHER_SEND_BYTES_THRESHOLD == 0
    fprintf(stderr,
            KLT_PUR "SendFlatBatcher is in command queue mode\n" KRESET);
#endif
    channels = new SendFlatBatchChannel*[mpi_size];
    for (auto i = 0; i < mpi_size; ++i) {
        if (i != my_mpi_rank) {
            channels[i] = new SendFlatBatchChannel(i);
        }
    }

    rank_send_queues = new SendQueue*[num_threads];
    for (auto i = 0; i < num_threads; ++i) {
        rank_send_queues[i] = new SendQueue(send_queue_size);
    }
}

SendFlatBatcher::~SendFlatBatcher() {
    for (auto i = 0; i < mpi_size; ++i) {
        if (i != mpi_rank) {
            delete (channels[i]);
        }
    }
    delete[](channels);

    for (auto i = 0; i < num_threads; ++i) {
        assert(rank_send_queues[i]->sizeGuess() == 0);
        delete (rank_send_queues[i]);
    }
    delete[](rank_send_queues);

    if (consume_req_q_empty + consume_req_q_non_empty > 0) {
        const auto sum = consume_req_q_empty + consume_req_q_non_empty;
        fprintf(stderr,
                "[m%d] SendFlatBatcher Stats: queue empty %llu / %llu (%f%%)\n",
                pure_process->GetMpiRank(), consume_req_q_empty, sum,
                percentage<uint64_t>(consume_req_q_empty, sum));
    } else {
        fprintf(stderr, "[m%d] This SendFlatBatcher had zero queue accesses\n",
                pure_process->GetMpiRank());
    }
}

void SendFlatBatcher::Finalize() {
    for (auto i = 0; i < mpi_size; ++i) {
        if (i != mpi_rank) {
            channels[i]->Finalize();
        }
    }
}

// Enqueues a message to this thread's exclusive queue (consumed by the batcher
// thread)
void SendFlatBatcher::EnqueueMessage(int thread_num, void* msg_payload,
                                     int payload_bytes, int sender_pure_rank,
                                     int          receiver_pure_rank,
                                     MPI_Datatype datatype, int tag) {

    // the goal here is to place the metadata about the message info the
    // PendingSend queue for this thread as quickly as possible
    const FlatBatcherHeader header{msg_payload,      payload_bytes,
                                   sender_pure_rank, receiver_pure_rank,
                                   datatype,         tag};
    const auto did_write = rank_send_queues[thread_num]->write(header);
    // fprintf(stderr, "send rank wrote message to queue %p with size %lu\n",
    //         rank_send_queues[thread_num],
    //         rank_send_queues[thread_num]->sizeGuess());

    // put on this for now
    check_always(did_write,
                 "EnqueueMessage: failed to write message into send queue; "
                 "it's capacity is currently %lu",
                 rank_send_queues[thread_num]->capacity());
}

int SendFlatBatcher::NumPendingEnqueues(int thread_num) const {
    return rank_send_queues[thread_num]->sizeGuess();
}

/////////////////////////////////////////////////////////////////////////
// Batcher Thread Functions

// The function that the batcher thread runs constantly (until it's killed)
void SendFlatBatcher::BatcherThreadRunner() {
    // loop through the send queues, putting messages onto the appropriate
    // channels based on the MPI endpoint
    char worker_name[16];
    sprintf(worker_name, "SendF_Bat_%.3d", mpi_rank);
    pthread_set_name(worker_name);

#ifdef CORI_ARCH
    fprintf(stderr, KRED "HACK FOR EVALUATION -- REMOVE THIS forcing recv "
                         "bacher to run on a forced core\n" KRESET);
    set_cpu_affinity_on_linux(46);
#endif

    int consec_empty_cnt = 0;

    while (pure_process->RunSendFlatBatcher()) {
        ConsumeEnqueueRequests(consec_empty_cnt);
    }
}

// Round-robin approach
#define RR_ENQUEUE 1
#if RR_ENQUEUE
void SendFlatBatcher::ConsumeEnqueueRequests(int& consec_empty_cnt) {

    // Consume as much as you can up to the send threshold. If you hit the
    // send threshold, send the batch. If you don't, exit and get more next
    // time.
    const int consec_empty_cnt_probe_limit = num_threads;

    for (auto t = 0; t < num_threads; ++t) {
        SendQueue* const         q = rank_send_queues[t];
        FlatBatcherHeader* const h = q->frontPtr();

        if (BATCHER_DEBUG_PRINT && SENDER_BATCHER_DEBUG_PRINT && h != nullptr)
            fprintf(stderr,
                    "%p ConsumeEnqueueReqs: trying to adding message from "
                    "rank %d for sending\n",
                    this, h->sender_pure_rank);

        if (h == nullptr) {
            // nothing left on thread t's queue
            ++consume_req_q_empty;
#if FLAT_BATCHER_DO_MPI_PROGRESS_ON_WAIT
            +consec_empty_cnt;
            if (consec_empty_cnt == consec_empty_cnt_probe_limit) {
                int        ignore;
                const auto ret =
                        MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD,
                                   &ignore, MPI_STATUS_IGNORE);
                assert(ret == MPI_SUCCESS);
                consec_empty_cnt = 0; // reset
            }
#endif
            // fprintf(stderr, "queue %p empty for thread %d\n", q, t);
            continue;
        } else {
            ++consume_req_q_non_empty;
            // fprintf(stderr, KGRN "queue %p non-empty for thread %d\n" KRESET,
            // q, t);
            const auto mpi_mate =
                    pure_process->MPIRankFromThreadRank(h->receiver_pure_rank);
#if FLAT_BATCHER_SEND_BYTES_THRESHOLD == 0
            // command-queue mode
            const auto ret =
                    MPI_Send(h->payload_buf, h->payload_bytes, MPI_BYTE,
                             mpi_mate, h->tag, MPI_COMM_WORLD);
            assert(ret == MPI_SUCCESS);
#else
            // batch mode
            channels[mpi_mate]->AddMessage(h);

#endif
            q->popFront();
        }
    } // for each thread
}
#else
// drain one queue at a time version -- probably not what we want
#error FLAT_BATCHER_DO_MPI_PROGRESS_ON_WAIT NYI
void SendFlatBatcher::ConsumeEnqueueRequests() {
    // Consume as much as you can up to the send threshold. If you
    // hit the send threshold, send the batch. If you don't, exit
    // and get more next time.

    // TODO: consdier pulling just one item from each thread to try
    // to be more fair.
    for (auto t = 0; t < num_threads; ++t) {
        SendQueue* const q = rank_send_queues[t];

        const auto sz = q->sizeGuess();
        if (sz > 0 && BATCHER_DEBUG_PRINT) {
            fprintf(stderr,
                    "%p ConsumeEnqueueRequests: t%d has %lu "
                    "entries in queue\n",
                    this, t, sz);
        }

        while (true) {
            // drain everything out of thread t's send queue as
            // possible
            FlatBatcherHeader* const h = q->frontPtr();
            // TODO: optimize reading a batch but have to deal with
            // wrap around
            if (h == nullptr) {
                break;
            } else {
                const auto mpi_mate = pure_process->MPIRankFromThreadRank(
                        h->receiver_pure_rank);
                if (BATCHER_DEBUG_PRINT)
                    fprintf(stderr,
                            "%p ConsumeEnqueueReqs: adding message "
                            "destined "
                            "for pure "
                            "rank %d to mpi_mate %d's channel for "
                            "sending\n",
                            this, h->receiver_pure_rank, mpi_mate);
                channels[mpi_mate]->AddMessage(h);
                q->popFront();
            } // while there's still more messages to read from
              // thread t
        }     // for all threads
    }
}
#endif
/////////////////////////////////////////////////////////////////////////
