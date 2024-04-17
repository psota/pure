// Author: James Psota
// File:   scan_channel.h 

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

#ifndef SCAN_CHANNEL_H
#define SCAN_CHANNEL_H

#include "pure.h"
#include "pure/runtime/pure_thread.h"
#include "pure/support/helpers.h"
#include <vector>

// each ScanChannel gets an automatically created tag. We assert that users
// don't create channels with tags above this value.
// each thread should have its own tag counter.
static thread_local int __running_tag = (PURE_INTERNAL_FIRST_RESERVED_TAG + 1);

template <typename ValType>
class ScanChannel {

  public:
    ScanChannel(PureThread* const pure_thread_arg, int count_arg,
                MPI_Datatype datatype_arg, MPI_Op op, bool inclusive,
                size_t max_buffered_msgs)
        : pure_thread(pure_thread_arg), count(count_arg),
          datatype(datatype_arg), op(op), inclusive(inclusive) {

        assert(pure_thread_arg != nullptr);

        const int comm_size = pure_thread->Get_size();
        const int rank      = pure_thread->Get_rank();

        // establish send channels -- send to everyone with rank larger than
        // mine
        const int num_send_chans = comm_size - rank - 1;
        send_chans.reserve(num_send_chans);

        // system-wide ScanChannels should keep the same __running_tag, but just
        // double check this
        const int scan_chan_tag = __running_tag + (inclusive ? 1 : 0);
        __running_tag += 2;

        // establish recv channels -- receive from everyone less than me. Note
        // that this has to be done first to allow the send channels to
        // initialize (their initialization requires the receive channel being
        // initialized).
        const int num_recv_chans = rank;
        recv_chans.reserve(num_recv_chans);

        for (auto s = 0; s < rank; ++s) {
            const bool using_rt_recv_buf_hint = true;
            recv_chans.push_back(pure_thread->InitRecvChannel(
                    count, datatype, s, scan_chan_tag, using_rt_recv_buf_hint,
                    max_buffered_msgs));
        }

        for (auto r = rank + 1; r < comm_size; ++r) {
            SendChannel* const sc = pure_thread->InitSendChannel(
                    nullptr, count, datatype, r, scan_chan_tag,
                    BundleMode::none, CHANNEL_ENDPOINT_TAG_SCAN_CHAN,
                    USER_SPECIFIED_BUNDLE_SZ_DEFAULT, false, false,
                    max_buffered_msgs);
            send_chans.push_back(sc);
        }
    }

    ~ScanChannel() = default;

    template <typename BufType = ValType>
    void Scan(BufType* send_buf, BufType* const recv_buf) {

        check(op == MPI_SUM, "Only sum is supported right now for ScanChannel");
        check(count * sizeof(BufType) <= BUFFERED_CHAN_MAX_PAYLOAD_BYTES,
              "Size of the ScanChannel (%d) must be less than "
              "BUFFERED_CHAN_MAX_PAYLOAD_BYTES (%d) for now as it's written in "
              "a "
              "way to assume the runtime buffers are used. If you ever need a "
              "really large scan channel, ScanChannel must be extended to "
              "support this.",
              count * sizeof(BufType), BUFFERED_CHAN_MAX_PAYLOAD_BYTES);

        // 1. initiate all dequeues
        for (auto rc : recv_chans) {
            // fprintf(stderr, KLT_PUR "ScanChannel: doing Dequeue on rc %p\n"
            // KRESET, rc);
            rc->Dequeue();
        }

        // 2. do all sends
        for (auto sc : send_chans) {
            sc->EnqueueUserBuf(send_buf);
            sc->Wait();
        }

        // 3a. initialize the recv buf
        if (inclusive) {
            // initialize with the send_buf directly
            MEMCPY_IMPL(recv_buf, send_buf, count * sizeof(BufType));
        } else {
            MEMSET_IMPL(recv_buf, 0, count * sizeof(BufType));
        }

        // 3b. wait on all receives, doing reduction upon receipt
        for (auto rc : recv_chans) {
            rc->Wait();

            // now, the rc's buffer is valid. reduce that value into the recv
            // buf.
            const BufType* msg_buf = rc->template GetRecvChannelBuf<BufType*>();
            for (auto i = 0; i < count; ++i) {
                recv_buf[i] += msg_buf[i];
            }
        }
    }

  private:
    std::vector<SendChannel*> send_chans;
    std::vector<RecvChannel*> recv_chans;

    PureThread*        pure_thread;
    const int          count;
    const MPI_Datatype datatype;
    const MPI_Op       op;
    const bool         inclusive;
};

#endif