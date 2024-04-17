// Author: James Psota
// File:   send_flat_batch_channel.cpp 

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

#include "pure/transport/experimental/send_flat_batch_channel.h"
#include "pure/transport/experimental/send_flat_batcher.h"
#include <algorithm>
#include <vector>

using std::vector;

SendFlatBatchChannel::SendFlatBatchChannel(int mate_mpi_rank_arg)
    : send_pending_check("send_pending_check"), do_isend("do_isend"),
      memcpy_timer("memcpy_timer") {

    mpi_structs.reserve(max_in_flight_sends);
    for (auto i = 0; i < max_in_flight_sends; ++i) {
        mpi_structs.push_back(new SendFlatBatchChannelMpi(mate_mpi_rank_arg));
    }

    size_bytes_max            = mpi_structs[0]->GetSizeBytesMax();
    size_bytes_send_threshold = send_bytes_threshold_factor * size_bytes_max;
}

SendFlatBatchChannel::~SendFlatBatchChannel() {
    std::for_each(mpi_structs.begin(), mpi_structs.end(),
                  [](SendFlatBatchChannelMpi* s) { delete (s); });

    send_pending_check.Print();
    do_isend.Print();
    memcpy_timer.Print();
}

void SendFlatBatchChannel::Finalize() {
    if (bytes_added_cume > 0) {
        SendAndReset();
    }
    check_always(bytes_added_cume == 0,
                 "Expected bytes_added_cume to be zero at the dcon of a send "
                 "channel -- otherwise these bytes won't be sent. There are %d "
                 "bytes unsent.\n",
                 bytes_added_cume);

    // now, go through each SendFlatBatchChannelMpi and finalize them as well --
    // we may have to wait for them to fully send
    std::for_each(mpi_structs.begin(), mpi_structs.end(),
                  [](SendFlatBatchChannelMpi* m) { m->Finalize(); });
}

void SendFlatBatchChannel::AddMessage(FlatBatcherHeader* const h) {
    const auto bytes_to_add =
            FlatBatcherHeader::SentSizeBytes() + h->payload_bytes;
    if (bytes_to_add > BytesAvailable()) {
        SendAndReset();
    }

    check(bytes_to_add <= BytesAvailable(),
          "Trying to add a message with %d header + payload bytes, but there "
          "are only "
          "%d bytes available",
          bytes_to_add, BytesAvailable());
    AddMsgToBatch(h);

    // Try again to send if this message happened to be large
    if (bytes_added_cume >= size_bytes_send_threshold) {
        SendAndReset();
    }
}

int SendFlatBatchChannel::BytesAvailable() const {
    return size_bytes_max - bytes_added_cume;
}

void SendFlatBatchChannel::AddMsgToBatch(FlatBatcherHeader* const h) {
    // copy in the header, but not the buf pointer
    void* curr_batch_buf = GetCurrMPIStruct()->batch_buf;

    START_TIMER(memcpy_timer);
    char* dest = static_cast<char*>(curr_batch_buf) + bytes_added_cume;

#if DEBUG_CHECK
    const int* hdr_src = &(h->start_word);
#else
    const int* hdr_src = &(h->payload_bytes);
#endif

    memcpy(dest, hdr_src, FlatBatcherHeader::SentSizeBytes());

    // copy in the payload
    dest += FlatBatcherHeader::SentSizeBytes();
    memcpy(dest, h->payload_buf, h->payload_bytes);

    PAUSE_TIMER(memcpy_timer);

    bytes_added_cume += FlatBatcherHeader::SentSizeBytes() + h->payload_bytes;
    if (BATCHER_DEBUG_PRINT && SENDER_BATCHER_DEBUG_PRINT)
        fprintf(stderr,
                "%p AddMsgToBatch: bytes_added_cume is now %d. "
                "size_bytes_send_threshold is %d and send_bytes_max is %d. "
                "FlatBatcherHeader::SentSizeBytes() = %d and payload bytes is "
                "%d\n",
                this, bytes_added_cume, size_bytes_send_threshold,
                size_bytes_max, FlatBatcherHeader::SentSizeBytes(),
                h->payload_bytes);
}

void SendFlatBatchChannel::SendAndReset() {
    START_TIMER(do_isend);
    GetCurrMPIStruct()->DoMPIIsend(bytes_added_cume);
    PAUSE_TIMER(do_isend);
    START_TIMER(send_pending_check);
    this->ConcludeMPIISend();
    PAUSE_TIMER(send_pending_check);
    if (BATCHER_DEBUG_PRINT && SENDER_BATCHER_DEBUG_PRINT)
        fprintf(stderr, "SendFlatBatchChannel did send and reset\n");
}

void SendFlatBatchChannel::ConcludeMPIISend() {
    PrintBatchedMessage(this, GetCurrMPIStruct()->batch_buf, bytes_added_cume,
                        true);
    auto next = curr_mpi_struct + 1;
    if (next == max_in_flight_sends) {
        curr_mpi_struct = 0;
    } else {
        curr_mpi_struct = next;
    }

    // assert that this MPI send is already done. I could potentially add more
    // entries here.
    if (FLAT_BATCHER_DO_NONBLOCKING_SEND_TEST) {
        const auto is_pending = GetCurrMPIStruct()->IsSendPending();
        check_always(
                is_pending == false,
                "Expected send to not be in a pending "
                "state as we want to issue a new send. May need to increase "
                "FLAT_BATCHER_MAX_IN_FLIGHT_MPI_SENDS (%d)",
                FLAT_BATCHER_MAX_IN_FLIGHT_MPI_SENDS);
    } else {
        // wait
        GetCurrMPIStruct()->WaitOnSendCompletion();
    }
    bytes_added_cume = 0;
}

SendFlatBatchChannelMpi* SendFlatBatchChannel::GetCurrMPIStruct() {
    return mpi_structs[curr_mpi_struct];
}

// static
void SendFlatBatchChannel::PrintBatchedMessage(void const* const batch_chan_ptr,
                                               void* batch_buf, int total_bytes,
                                               bool is_sender) {
#if BATCHER_DEBUG_PRINT
    // iterate through the message, piece by piece
    // assume int payload for now -- templatize later
    void* curr_buf        = batch_buf;
    int   bytes_processed = 0;

    if (is_sender) {
        fprintf(stderr,
                KGREY "%p BATCHED MESSAGE FROM SEND BATCHER THREAD "
                      "---------------->\n" KRESET,
                batch_chan_ptr);
    } else {
        fprintf(stderr,
                KGREY KUNDER
                "----------------> %p BATCHED MESSAGE TO RECV BATCHER THREAD "
                "\n" KRESET,
                batch_chan_ptr);
    }

    while (bytes_processed < total_bytes) {
        FlatBatcherHeaderSend* h =
                static_cast<FlatBatcherHeaderSend*>(curr_buf);
        check(h->start_word == FLAT_BATCHER_HEADER_NONCE,
              "expected nonce, but is %d",
              h->start_word); // debug mode only

        const char spacer = is_sender ? ' ' : '\t';
        fprintf(stderr, KYEL "%c   [ %dB  r%d -> r%d  d:%d  tag:%d \t| " KRESET,
                spacer, h->payload_bytes, h->sender_pure_rank,
                h->receiver_pure_rank, static_cast<int>(h->datatype), h->tag);

        curr_buf = static_cast<char*>(curr_buf) +
                   FlatBatcherHeader::SentSizeBytes();
        bytes_processed += FlatBatcherHeader::SentSizeBytes();

        const auto num_ints = h->payload_bytes / sizeof(int);
        for (auto p = 0; p < num_ints; ++p) {
            fprintf(stderr, "%d:%u ", p, static_cast<int*>(curr_buf)[p]);
        }
        fprintf(stderr, "]\n");

        curr_buf = static_cast<char*>(curr_buf) + h->payload_bytes;
        bytes_processed += h->payload_bytes;
        assert(bytes_processed <= total_bytes);
    }
#endif
}