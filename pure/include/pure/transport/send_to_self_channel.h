// Author: James Psota
// File:   send_to_self_channel.h 

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

#ifndef SEND_TO_SELF_CHANNEL_H
#define SEND_TO_SELF_CHANNEL_H

#define INIT_SENTINEL -1

class SendToSelfChannel {

  public:
    void EnqueueUserBuf(void* user_buf, int bytes_to_send) {
        if (recv_buf == nullptr) {
            assert(user_buf != nullptr);
            send_buf   = user_buf;
            send_bytes = bytes_to_send;
            // fprintf(stderr,
            //         KGRN "%p Doing S2S Enqueue -- Enqeuue is first (sending
            //         %d "
            //              "bytes)\n" KRESET,
            //         this, bytes_to_send);
        } else {
            // receiver has already called Dequeue, so do the copy now
            assert(recv_max_bytes != INIT_SENTINEL);
            assert(bytes_to_send <= recv_max_bytes);
            memcpy(recv_buf, user_buf, bytes_to_send);
            Reset();
            // fprintf(stderr,
            //         KGRN "%p Doing S2S Enqueue -- Enqeuue is SECOND (sending
            //         "
            //              "%d bytes)\n" KRESET,
            //         this, bytes_to_send);
        }
    }

    void Dequeue(void* user_buf, int max_bytes_to_recv) {
        if (send_buf == nullptr) {
            recv_buf       = user_buf;
            recv_max_bytes = max_bytes_to_recv;
            // fprintf(stderr,
            //         KCYN "%p Doing S2S Dequeue -- Dequeeu is first (receiving
            //         "
            //              "at most %d bytes)\n" KRESET,
            //         this, max_bytes_to_recv);
        } else {
            assert(user_buf != nullptr);
            assert(send_bytes <= max_bytes_to_recv);
            memcpy(user_buf, send_buf, send_bytes);
            Reset();
            // fprintf(stderr,
            //         KCYN "%p Doing S2S Dequeue -- Dequeue is SECOND
            //         (receiving "
            //              "at most %d bytes)\n" KRESET,
            //         this, max_bytes_to_recv);
        }
    }

    void DequeueWait() {
        // here we want to make sure that the data transfer has actually been
        // done.
        // fprintf(stderr, KRED "%p Doing S2S Dequeue WAIT\n" KRESET, this);
        check(recv_buf == nullptr,
              "It seems that this send to self call was not done in the proper "
              "order. The Enqueue must be called before the receive->Wait() is "
              "called to ensure the receive buffer is valid");
    }

  private:
    void*  send_buf       = nullptr;
    void*  recv_buf       = nullptr;
    size_t send_bytes     = INIT_SENTINEL;
    size_t recv_max_bytes = INIT_SENTINEL;

    /////////////////////////////////////
    void Reset() {
        send_buf       = nullptr;
        recv_buf       = nullptr;
        send_bytes     = INIT_SENTINEL;
        recv_max_bytes = INIT_SENTINEL;
    }
};

#endif