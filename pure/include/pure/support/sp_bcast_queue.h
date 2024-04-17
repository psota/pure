// Author: James Psota
// File:   sp_bcast_queue.h 

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

/*
 * Custom concurrent queue with a single producer and multiple consumers. All
 * consumers must consume each buffer before the producer is able to reuse the
 * slow.
 */

#pragma once

#include <atomic>
#include <cassert>
#include <cstdlib>
#include <memory>
#include <new> // for CACHE_LINE_BYTES
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "pure/support/helpers.h"

using PureRT::__assert_cacheline_aligned;
using PureRT::round_up;

using AtomicIndex = std::atomic<unsigned int>;

#define START_NOT_CONSUMED_FLAG 0
#define START_CONSUMED_FLAG 1

struct PaddedAtomicFlag {
    // https://stackoverflow.com/questions/9806200/how-to-atomically-negate-an-stdatomic-bool
    alignas(CACHE_LINE_BYTES) std::atomic<unsigned int> flag;
    char pad[CACHE_LINE_BYTES - sizeof(std::atomic<unsigned int>)];
};

// force off for now.
#define DO_DEBUG_PRINT    \
    if (DEBUG_CHECK && 0) \
    fprintf

struct SingleProducerBcastQueue {

    SingleProducerBcastQueue(const SingleProducerBcastQueue&) = delete;
    SingleProducerBcastQueue&
    operator=(const SingleProducerBcastQueue&) = delete;

    // size must be >= 2.
    //
    // Also, note that the number of usable slots in the queue at any
    // given time is actually (size-1), so if you start with an empty queue,
    // isFull() will return true after size-1 insertions.
    explicit SingleProducerBcastQueue(uint32_t size, uint32_t bytes_per_payload,
                                      uint32_t num_consumers)

        : size_(size), bytes_per_payload_(bytes_per_payload),
          rounded_bytes_per_payload_(
                  get_rounded_bytes_per_payload(bytes_per_payload)),
          buffers_(jp_memory_alloc<1, CACHE_LINE_BYTES, 1>(calculate_buf_size(
                  size, get_rounded_bytes_per_payload(bytes_per_payload)))),
          num_consumers_(num_consumers),
          curr_done_consuming_flag(START_CONSUMED_FLAG), writeIndex_(0) {
        assert(size >= 3);
        assert(num_consumers >= 0); // this could be a dummy object
        assert(bytes_per_payload > 0);

        if (num_consumers == 0) {
            // just return -- this would be the case when we are running in
            // PureSingleThread mode and this data structure can never be used.
            return;
        }

        PureRT::assert_cacheline_aligned(buffers_);
        if (!buffers_) {
            throw std::bad_alloc();
        }

        remaining_counts_ =
                jp_memory_alloc<1, CACHE_LINE_BYTES, 0, PaddedAtomicFlag*>(
                        sizeof(PaddedAtomicFlag*) * size);

        for (auto i = 0; i < size; ++i) {
            // the leader is always thread zero, so this gets indexed using the
            // non-leaders thread num minus one
            remaining_counts_[i] =
                    jp_memory_alloc<1, CACHE_LINE_BYTES, 0, PaddedAtomicFlag>(
                            sizeof(PaddedAtomicFlag) * num_consumers);
            for (auto c = 0; c < num_consumers_; ++c) {
                // initialize as done, as they are "done" on the first pass
                // through the queue.
                remaining_counts_[i][c].flag.store(START_CONSUMED_FLAG);
            }
        }

        // zero out data to ensure zero consumers remaining on all
        // entries
        std::memset(buffers_, 0, calculate_buf_size(size, bytes_per_payload));

        DO_DEBUG_PRINT(
                stderr,
                KYEL "SPBQ details: size %d  bytes per payload: %d  consumers: "
                     "%d  rounded_bytes_per_payload: %d"
                     "  \n" KRESET,
                size_, bytes_per_payload_, num_consumers_,
                rounded_bytes_per_payload_);
    }

    ~SingleProducerBcastQueue() {
        std::free(buffers_);

        if (num_consumers_ > 0) {
            for (auto i = 0; i < size_; ++i) {
                delete[](remaining_counts_[i]);
            }
            delete[](remaining_counts_);
        }
    }

    uint32_t calculate_buf_size(uint32_t size, uint32_t bytes_per_payload) {
        return bytes_per_payload * size;
    }

    uint32_t get_rounded_bytes_per_payload(uint32_t bytes_per_payload) {
        return round_up(bytes_per_payload, CACHE_LINE_BYTES);
    }

    void* get_buf_ptr(unsigned int index) {
        const auto ret =
                static_cast<void*>(static_cast<char*>(buffers_) +
                                   (index * rounded_bytes_per_payload_));
        // DO_DEBUG_PRINT(stderr, "buf_ptr: %p\n", ret);
        return ret;
    }

    // blocks until it can push
    void push_bcast_data(void const* const __restrict src_buf) {
        // two waits must occur -- wait for slot to be free, and wait for that
        // slot to be fully consumed by all consumers.
        auto const currentWrite = writeIndex_.load(std::memory_order_acquire);
        DO_DEBUG_PRINT(stderr,
                       KRED
                       "push bcast:\t want to write into index: %d\n" KRESET,
                       currentWrite);

        DO_DEBUG_PRINT(stderr,
                       KRED "\tpush bcast:\t done waiting on slot\n" KRESET);

        // now entry is writable; write it!
        void* const dest_buf = get_buf_ptr(currentWrite);
        assert_cacheline_aligned(dest_buf);
        MEMCPY_IMPL(__builtin_assume_aligned(dest_buf, CACHE_LINE_BYTES),
                    src_buf, bytes_per_payload_);

        DO_DEBUG_PRINT(stderr,
                       KRED
                       "\tpush bcast:\t did memcpy; writeIndex_ now %d\tfirst "
                       "word written is %d to buf %p\n" KRESET,
                       currentWrite, ((int*)(src_buf))[0], src_buf);

        // indicate that this entry is readable
        auto nextRecord = currentWrite + 1;
        if (nextRecord == size_) {
            nextRecord = 0;
            curr_done_consuming_flag ^= 1; // flip it
            DO_DEBUG_PRINT(stderr,
                           KCYN "Flipping curr done flag -- now is %d and "
                                "nextIndex is %d\n" KRESET,
                           curr_done_consuming_flag, nextRecord);
        }

        // but first, wait until we know that all of nextRecord has been
        // consumed spin until slot has been consumed by all consumers
        for (auto i = 0; i < num_consumers_; ++i) {
            while (remaining_counts_[nextRecord][i].flag.load(
                           std::memory_order_acquire) !=
                   curr_done_consuming_flag) {
                x86_pause_instruction();
            }
        }

        writeIndex_.store(nextRecord, std::memory_order_release);
    }

    // blocks until data available
    void copy_out_bcast_data(void* const __restrict dest_buf, int pure_rank,
                             int my_thread_num, uint32_t& my_read_index) {

        DO_DEBUG_PRINT(stderr,
                       KGRN "r%d  starting copy_out_bcast_data:\t "
                            "my_read_index %d\n" KRESET,
                       pure_rank, my_read_index);

        // wait until queue is not empty -- which means a slot is readable
        while (my_read_index == writeIndex_.load(std::memory_order_acquire)) {
            x86_pause_instruction();
            // work steal?
        }

        DO_DEBUG_PRINT(
                stderr,
                KGRN
                "\tr%d  copy_out_bcast_data:\t done waiting for slot %d to be "
                "readable\n" KRESET,
                pure_rank, my_read_index);

        // data is available! now, copy it out and decrement consumers remaining
        void* const src_buf = get_buf_ptr(my_read_index);
        assert_cacheline_aligned(src_buf);
        MEMCPY_IMPL(dest_buf,
                    __builtin_assume_aligned(src_buf, CACHE_LINE_BYTES),
                    bytes_per_payload_);

        // update the sense-reversing flag that I have consumed my entry
        // we index in with thread num minus one, as thread 0 is the leader
        const auto prev_flag =
                remaining_counts_[my_read_index][my_thread_num - 1]
                        .flag.fetch_xor(1); // this atomically
                                            // inverts it
        assert(prev_flag == 0 || prev_flag == 1);

        DO_DEBUG_PRINT(stderr,
                       KGRN
                       "\tr%d  copy_out_bcast_data (first word: %d from buf "
                       "%p):\t did "
                       "memcpy. flipped done flag from %d to %d\n" KRESET,
                       pure_rank, ((int*)(src_buf))[0], src_buf, prev_flag,
                       prev_flag ^ 1);

        // pop off this entry as I'm the last consumer of it
        my_read_index = my_read_index + 1;
        if (my_read_index == size_) {
            my_read_index = 0;
        }

        DO_DEBUG_PRINT(stderr,
                       KGRN
                       "\tr%d  copy_out_bcast_data:\t updated my_read_index to "
                       "%d\n" KRESET,
                       pure_rank, my_read_index);
    }

    // maximum number of items in the queue.
    size_t capacity() const { return size_ - 1; }

  private:
    char               pad0_[CACHE_LINE_BYTES];
    void* const        buffers_;
    PaddedAtomicFlag** remaining_counts_;
    const uint32_t     size_;
    const uint32_t     bytes_per_payload_;
    const uint32_t     rounded_bytes_per_payload_;
    const uint32_t     num_consumers_;
    unsigned int       curr_done_consuming_flag;

    alignas(CACHE_LINE_BYTES) AtomicIndex writeIndex_;

    char pad1_[CACHE_LINE_BYTES - sizeof(AtomicIndex)];
};
