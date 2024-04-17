// Author: James Psota
// File:   recv_flat_batcher_v2.cpp 

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

#include "pure/transport/experimental/v2/recv_flat_batcher_v2.h"
#include "mpi.h"
#include "pure/runtime/pure_process.h"
#include "pure/support/helpers.h"
#include "pure/transport/experimental/send_flat_batch_channel.h"
#include "pure/transport/experimental/v2/recv_flat_batch_channel_v2.h"
#include "pure/transport/recv_channel.h"

RecvFlatBatcher::RecvFlatBatcher(PureProcess* const pp, int mpi_size_arg,
                                 int my_mpi_rank, int num_threads_arg)
    : pure_process(pp), mpi_size(mpi_size_arg), mpi_rank(my_mpi_rank),
      num_threads(num_threads_arg) {

#if FLAT_BATCHER_SEND_BYTES_THRESHOLD == 0
    recv_buf = jp_memory_alloc<1, CACHE_LINE_BYTES, 1>(payload_bytes_max,
                                                       &payload_bytes_max);
#endif
#if FLAT_BATCHER_SEND_BYTES_THRESHOLD > 0
    channels = new RecvFlatBatchChannel*[mpi_size];
    for (auto i = 0; i < mpi_size; ++i) {
        if (i != my_mpi_rank) {
            channels[i] = new RecvFlatBatchChannel(i);
        }
    }
#endif

    recv_queues = new RecvQueue*[num_threads];
    for (auto i = 0; i < num_threads; ++i) {
        recv_queues[i] = new RecvQueue(recv_queue_size);
    }

    posted_lists     = new PendingList**[num_threads];
    unexpected_lists = new UnexpectedList**[num_threads];
    for (auto d = 0; d < num_threads; ++d) {
        // the dest thread num ones -- disambiguates between threads
        posted_lists[d]     = new PendingList*[num_threads];
        unexpected_lists[d] = new UnexpectedList*[num_threads];
        for (auto s = 0; s < num_threads; ++s) {
            // the sender thread num ones
            posted_lists[d][s] = new PendingList();
            posted_lists[d][s]->reserve(FLAT_BATCHER_INIT_RECV_RANK_LISTS_SZ);
            unexpected_lists[d][s] = new UnexpectedList();
            unexpected_lists[d][s]->reserve(
                    FLAT_BATCHER_INIT_RECV_RANK_LISTS_SZ);
        }
    }
}

RecvFlatBatcher::~RecvFlatBatcher() {

    if (wait_read_my_queue_try_t0 > 0) {
        fprintf(stderr,
                "thread 0: success/try wait read from queue: %llu / %llu  "
                "(%f)%%\n",
                wait_read_my_queue_success_t0, wait_read_my_queue_try_t0,
                percentage<uint64_t>(wait_read_my_queue_success_t0,
                                     wait_read_my_queue_try_t0));
    }

#if FLAT_BATCHER_SEND_BYTES_THRESHOLD == 0
    free(recv_buf);
#endif
#if FLAT_BATCHER_SEND_BYTES_THRESHOLD > 0
    // assert size of queues is zero
    for (auto i = 0; i < mpi_size; ++i) {
        if (i != mpi_rank) {
            delete (channels[i]);
        }
    }
    delete[](channels);
#endif

    for (auto i = 0; i < num_threads; ++i) {
        delete (recv_queues[i]);
    }
    delete[](recv_queues);

    for (auto d = 0; d < num_threads; ++d) {
        for (auto s = 0; s < num_threads; ++s) {
            delete (posted_lists[d][s]);
            delete (unexpected_lists[d][s]);
        }
        delete[](posted_lists[d]);
        delete[](unexpected_lists[d]);
    }
    delete[](posted_lists);
    delete[](unexpected_lists);
}

void RecvFlatBatcher::Finalize() {
#if FLAT_BATCHER_SEND_BYTES_THRESHOLD > 0
    for (auto i = 0; i < mpi_size; ++i) {
        if (i != mpi_rank) {
            channels[i]->Finalize();
        }
    }
#endif
}

/////////////////////////////////////////////////////////////////////////

void RecvFlatBatcher::PostReceiveRequest(int                dest_thread_num,
                                         RecvChannel* const rc, void* dest_buf,
                                         int payload_bytes_max,
                                         int sender_pure_rank,
                                         int dest_pure_rank, MPI_Datatype dt,
                                         int tag) {

    PendingList* const sender_posted_list =
            GetPendingRecvList(sender_pure_rank, dest_thread_num);
    sender_posted_list->emplace_back(rc, dest_buf, payload_bytes_max, dt, tag);

    if (0)
        fprintf(stderr,
                "  r%d\tpushed posted recv req for tag %d onto sender r%d "
                "posted list\n",
                dest_pure_rank, tag, sender_pure_rank);
}

void RecvFlatBatcher::WaitRecvRequest(int                dest_thread_num,
                                      RecvChannel* const rc, void* dest_buf,
                                      int payload_bytes_max,
                                      int sender_pure_rank, int dest_pure_rank,
                                      MPI_Datatype dt, int tag) {

    // 1. See if this message is already done?
    if (rc->FlatBatcherMsgCompleted()) {
        if (RECEIVER_BATCHER_DEBUG_PRINT)
            fprintf(stderr,
                    KGRN "  r%d\tWait: msg already completed: tag %d, sender "
                         "r%d\n" KRESET,
                    dest_pure_rank, tag, sender_pure_rank);
        return;
    }

    // 2. See if we can match this message to the unexpected list
    UnexpectedList* const sender_unexpected_list =
            GetUnexpectedRecvList(sender_pure_rank, dest_thread_num);
    const RecvInstructions needle{dt, tag};
    auto unexpected_iter = std::find(sender_unexpected_list->begin(),
                                     sender_unexpected_list->end(), needle);

    if (unexpected_iter != sender_unexpected_list->end()) {
        // did find the message I was looking for on the unexpected list.
        // Consume and complete it. Then, return as the message I was looking
        // for was here.
        ConsumeAndCompleteMsg(
                unexpected_iter->msg_buf, unexpected_iter->payload_bytes,
                unexpected_iter->mpi_struct, dest_buf, payload_bytes_max, rc);
        sender_unexpected_list->erase(unexpected_iter);

        const int payload_zero = static_cast<int*>(unexpected_iter->msg_buf)[0];
        if (RECEIVER_BATCHER_DEBUG_PRINT)
            fprintf(stderr,
                    KGRN
                    "  r%d\tWait: found msg [%d] in unexpected list: tag %d, "
                    "sender r%d\n" KRESET,
                    dest_pure_rank, payload_zero, tag, sender_pure_rank);
        return;
    }

    // 3. If I didn't find the message I wanted in the unexpected list, drain my
    // recv_queue until I find it. For RecvInstructions that don't match what I
    // want, first try to find it in the posted_list. If I match there, consume
    // & complete the message. If I don't match there, put it on the unexpected
    // list.
    RecvQueue* const my_recv_q = recv_queues[dest_thread_num];

    while (true) {
        // 1. pull out a message from my recv queue
        AlignedRecvInstructions queue_recv_inst;
        const auto              got_inst = my_recv_q->read(queue_recv_inst);

        if (dest_thread_num == 0)
            ++wait_read_my_queue_try_t0;

        if (got_inst) {
            if (dest_thread_num == 0)
                ++wait_read_my_queue_success_t0;

            // only in debugging mode does the inst have the dest rank
            assert(queue_recv_inst.receiver_pure_rank == dest_pure_rank);

            // 2. does it match something in the pending list?
            // if so, go ahead and C&C it and remove from pending, of course
            const auto inst_sender_thread_num =
                    pure_process->ThreadNumOnNodeFromPureRank(
                            queue_recv_inst.sender_pure_rank);
            PendingList* const sender_pending_list =
                    GetPendingRecvList(inst_sender_thread_num, dest_thread_num);

            const RecvReq pending_needle{queue_recv_inst.datatype,
                                         queue_recv_inst.tag};
            const auto    pending_match_iter =
                    std::find(sender_pending_list->begin(),
                              sender_pending_list->end(), pending_needle);

            const int payload_zero =
                    static_cast<int*>(queue_recv_inst.msg_buf)[0];

            if (pending_match_iter == sender_pending_list->end()) {
                // didn't match anything in the pending list, so put it on the
                // unexpected list
                UnexpectedList* const sender_unexpected_list =
                        GetUnexpectedRecvList(inst_sender_thread_num,
                                              dest_thread_num);
                sender_unexpected_list->emplace_back(queue_recv_inst);

                // assert that this message isn't the one we wnat
                assert(queue_recv_inst.Matches(sender_pure_rank, dt, tag) ==
                       false);

                if (RECEIVER_BATCHER_DEBUG_PRINT)
                    fprintf(stderr,
                            KYEL
                            "  r%d\tWait: got an unexpected msg [%d] from recv "
                            "queue -- putting it on "
                            "unexpected list: tag %d, "
                            "sender r%d. sender_unexpected_list size: "
                            "%d\n" KRESET,
                            dest_pure_rank, payload_zero, queue_recv_inst.tag,
                            queue_recv_inst.sender_pure_rank,
                            sender_unexpected_list->size());

            } else {
                // the incoming queue inst matched an entry in the pending list
                // 1. C&C it
                ConsumeAndCompleteMsg(queue_recv_inst.msg_buf,
                                      queue_recv_inst.payload_bytes,
                                      queue_recv_inst.mpi_struct,
                                      pending_match_iter->dest_buf,
                                      pending_match_iter->payload_bytes,
                                      pending_match_iter->rc);

                // 2. remove it from the pending list
                sender_pending_list->erase(pending_match_iter);

                // 3. is this the one I wanted? If so, return. It's already been
                // C&C'd.
                if (queue_recv_inst.Matches(sender_pure_rank, dt, tag)) {
                    if (RECEIVER_BATCHER_DEBUG_PRINT)
                        fprintf(stderr,
                                KGRN "  r%d\tWait: found msg [%d] directly in "
                                     "queue: "
                                     "tag %d, "
                                     "sender r%d. sender_pending_list size: "
                                     "%d\n" KRESET,
                                dest_pure_rank, payload_zero, tag,
                                sender_pure_rank, sender_pending_list->size());
                    return;
                } else {
                    if (RECEIVER_BATCHER_DEBUG_PRINT)
                        fprintf(stderr,
                                KYEL "  r%d\tWait: matched a message [%d] ]in "
                                     "pending "
                                     "and "
                                     "consumed it: tag %d, "
                                     "sender r%d. sender_pending_list size: "
                                     "%d\n" KRESET,
                                dest_pure_rank, payload_zero,
                                queue_recv_inst.tag,
                                queue_recv_inst.sender_pure_rank,
                                sender_pending_list->size());
                }
            } // matched unexpected
        }     // if we got an element off the incoming recv queue
    }         // while true -- keep looping until we find the desired message
}

PendingList* const
RecvFlatBatcher::GetPendingRecvList(int sender_pure_rank,
                                    int dest_thread_num) const {
    const auto send_thread_num =
            pure_process->ThreadNumOnNodeFromPureRank(sender_pure_rank);
    return posted_lists[dest_thread_num][send_thread_num];
}

UnexpectedList* const
RecvFlatBatcher::GetUnexpectedRecvList(int sender_pure_rank,
                                       int dest_thread_num) const {
    const auto send_thread_num =
            pure_process->ThreadNumOnNodeFromPureRank(sender_pure_rank);
    return unexpected_lists[dest_thread_num][send_thread_num];
}

void RecvFlatBatcher::ConsumeAndCompleteMsg(void* const msg_buf,
                                            int         inst_payload_bytes,
                                            RecvFlatBatchChannelMpi* mpi_struct,
                                            void* const              dest_buf,
                                            int payload_bytes_max,
                                            RecvChannel* const rc) {
    // 1. Consume the message into my buffer
    check_always(inst_payload_bytes <= payload_bytes_max,
                 "Expected incoming message payload size to be smaller or "
                 "equal to receiver's max buf size");
    memcpy(dest_buf, msg_buf, inst_payload_bytes);

    // 2. Complete it with respect to the MPI channel and the
    // RecvChannel
    mpi_struct->MarkMsgConsumed();
    rc->SetReceivedCount(inst_payload_bytes);
    rc->MarkFlatBatcherMsgCompleted();
}

/////////////////////////////////////////////////////////////////////////
// Batcher Thread Functions

// The function that the batcher thread runs constantly
void RecvFlatBatcher::BatcherThreadRunner() {
    char worker_name[16];
    sprintf(worker_name, "RecvF_Bat_%.3d", mpi_rank);
    pthread_set_name(worker_name);

#ifdef CORI_ARCH
    fprintf(stderr, KBOLD KRED "HACK FOR EVALUATION -- REMOE THIS forcing send "
                               "bacher to run on a forced core\n" KRESET);
    set_cpu_affinity_on_linux(47);
#endif

    while (pure_process->RunRecvFlatBatcher()) {
#if FLAT_BATCHER_SEND_BYTES_THRESHOLD == 0
        RecvAndProcessDirectMessages();
#else
        RecvAndProcessBatchMessages();
#endif
    }
}

void RecvFlatBatcher::RecvAndProcessBatchMessages() {
    // loop through all of the MPI endpoints
    for (auto m = 0; m < mpi_size; ++m) {
        if (m == mpi_rank) {
            continue;
        }

        void*                    received_batch_buf;
        int                      bytes_received;
        RecvFlatBatchChannelMpi* this_mpi_struct;
        channels[m]->TryGetMsgBatchBuf(&received_batch_buf, &bytes_received,
                                       &this_mpi_struct);

        if (received_batch_buf != nullptr) {
            assert(bytes_received > 0);
            SendFlatBatchChannel::PrintBatchedMessage(this, received_batch_buf,
                                                      bytes_received, false);
            PlaceBatchBufMsgsIntoRecvQueues(received_batch_buf, bytes_received,
                                            this_mpi_struct);
            channels[m]->MoveReceivedMpiStructToBeingConsumed();
        }

        // move back any consumed mpi_structs
        channels[m]->MigrateConsumedMessagesToBeingReceived();
    }
}

void RecvFlatBatcher::RecvAndProcessDirectMessages() {
#if FLAT_BATCHER_SEND_BYTES_THRESHOLD == 0
    int        test;
    const auto ret = MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD,
                                &test, MPI_STATUS_IGNORE);
    assert(ret == MPI_SUCCESS);
    if (test) {
        const auto ret =
                MPI_Recv(recv_buf, payload_bytes_max, MPI_BYTE, MPI_ANY_SOURCE,
                         MPI_ANY_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        assert(ret == MPI_SUCCESS);
    }
#endif
}

// Go through the batch buf, placing the instructions for this
// recv into the proper location
void RecvFlatBatcher::PlaceBatchBufMsgsIntoRecvQueues(
        void* received_batch_buf, int total_bytes,
        RecvFlatBatchChannelMpi* const this_mpi_struct) {

#if FLAT_BATCHER_RECV_WORKER_DROP_MESSAGES_ON_FLOOR
    return;
#endif

    int   bytes_processed = 0;
    void* curr_buf        = received_batch_buf;
    int   num_msgs        = 0;

    while (bytes_processed < total_bytes) {
        // process header
        auto h = static_cast<FlatBatcherHeaderSend*>(curr_buf);
        check(h->start_word == FLAT_BATCHER_HEADER_NONCE,
              "expected nonce (%d), but is %d", FLAT_BATCHER_HEADER_NONCE,
              h->start_word); // debug mode only

        // copy the payload into this dest buf
        void* msg_buf = static_cast<char*>(curr_buf) +
                        FlatBatcherHeader::SentSizeBytes();

        // place Receive Instructions into the receive queue for
        // this thread number
        const AlignedRecvInstructions inst {
            this_mpi_struct, msg_buf, h->payload_bytes, h->sender_pure_rank,
#if DEBUG_CHECK
                    h->receiver_pure_rank,
#endif
                    h->datatype, h->tag
        };

        const int receiver_thread_num =
                pure_process->ThreadNumOnNodeFromPureRank(
                        h->receiver_pure_rank);

        bool did_write;
        if (FLAT_BATCHER_RECV_WORKER_WAIT_ON_RECV_QUEUE_WRITE) {
            // keep spinning until
            while (recv_queues[receiver_thread_num]->write(inst) == false)
                ;
            did_write = true;
        } else {
            did_write = recv_queues[receiver_thread_num]->write(inst);
        }

#if DEBUG_CHECK
        if (!did_write) {
            sentinel("Recv worker tried to write message into rank %d's "
                     "recv_queue but it was full (capacity is %d)\n",
                     h->receiver_pure_rank,
                     recv_queues[receiver_thread_num]->capacity());
        }
#endif
#if FLAT_BATCHER_RECV_WORKER_WAIT_ON_RECV_QUEUE_WRITE == 0
        check_always(did_write,
                     "Recv worker tried to write message into recv queue but "
                     "it was full. You probably need to increase the queue "
                     "size or introduce waiting. Capacity is currently %lu.",
                     recv_queues[receiver_thread_num]->capacity());
#endif

        /////////////
        bytes_processed +=
                FlatBatcherHeader::SentSizeBytes() + h->payload_bytes;

        // advance to the next message
        curr_buf = static_cast<char*>(msg_buf) + h->payload_bytes;
        assert(bytes_processed <= total_bytes);

        ++num_msgs;
    }

    // now that we've placed all of the messages into receive queues, set
    // the number of unconsumed messages in the mpi_struct
    this_mpi_struct->AddNumMsgsInBatch(num_msgs);
}
