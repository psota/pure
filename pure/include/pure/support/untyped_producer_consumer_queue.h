// Author: James Psota
// File:   untyped_producer_consumer_queue.h 

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
 * Inspired by Facebook Folly's ProducerConsumerQueue.h
 */
#pragma once

#include <atomic>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "pure/common/pure_rt_enums.h"
#include "pure/support/benchmark_timer.h"
#include "pure/support/helpers.h"
#include "pure/support/zed_debug.h"

#if USE_ASMLIB
#include "pure/3rd_party/asmlib/asmlib.h"
#endif

#if COLLECT_THREAD_TIMELINE_DETAIL
#define START_MEMCPY_TIMER memcpy_timer.start()
#define PAUSE_MEMCPY_TIMER memcpy_timer.pause(-1)
#else
#define START_MEMCPY_TIMER
#define PAUSE_MEMCPY_TIMER
#endif

// experimental version that doesn't have an aligned payload

// The data structure returned from frontPtr has the following form:
//
//    [ payload_count      |  void* buf ]
//            ↑                   ↑
//         uint32_t      bytes_per_payload_buf
//   .................... or less if the enqueue gave a smaller amount

static unsigned constexpr buf_ptr_offset = sizeof(payload_count_type);
using sized_buf_ptr                      = void*;

/* this structure isn't actually used directly.

struct SizedBufPtr {
    payload_count_type payload_count;
    PureRT::buffer_t   buf_ptr;
};
*/

/*
 * ProducerConsumerQueue is a one producer and one consumer queue
 * without locks.
 */
struct UntypedProducerConsumerQueue {

    UntypedProducerConsumerQueue(const UntypedProducerConsumerQueue&) = delete;
    UntypedProducerConsumerQueue&
    operator=(const UntypedProducerConsumerQueue&) = delete;

    // size must be >= 2.
    //
    // Also, note that the number of usable slots in the queue at any
    // given time is actually (size-1), so if you start with an empty queue,
    // isFull() will return true after size-1 insertions.

    UntypedProducerConsumerQueue(const uint32_t size,
                                 const uint32_t bytes_per_payload_entry)
        : size_(size), bytes_per_payload_buf(bytes_per_payload_entry)
#if PCV3_OPTION_USE_PADDED_RECORDS
          ,
          padded_bytes_per_payload_buf(
                  calculateTotalBytesPerEntry(bytes_per_payload_entry))
#endif
          ,
          readIndex_(0), writeIndex_(0) {
        assert(size_ >= 2);
#if PCV3_OPTION_USE_PADDED_RECORDS
        const bool use_padded_records = true;
#else
        sentinel(
                "PCV3_OPTION_USE_PADDED_RECORDS=0 is not currently supported.");
        const bool use_padded_records = false;
#endif

        uint32_t records_bytes =
                calculateTotalBytesPerEntry(bytes_per_payload_buf) * size_;
        const auto allocated_bytes =
                jp_memory_alloc<use_padded_records, CACHE_LINE_BYTES, true>(
                        &records_, records_bytes);
        assert(allocated_bytes >= records_bytes);
        if (!records_) {
            throw std::bad_alloc();
        }

        PureRT::assert_cacheline_aligned(records_);
        assert(padded_bytes_per_payload_buf % CACHE_LINE_BYTES == 0);

#if PCV3_OPTION_PREVENT_MEMBER_FALSE_SHARING
        // assert that the padded members are padded sufficiently
        // assert(reinterpret_cast<uintptr_t>(&readIndex_) -
        //                reinterpret_cast<uintptr_t>(&pad0_) >=
        //        CACHE_LINE_BYTES);
        assert(reinterpret_cast<uintptr_t>(&writeIndex_) -
                       reinterpret_cast<uintptr_t>(&readIndex_) ==
               2 * CACHE_LINE_BYTES);

// My assumptions seem wrong here. See followup on
// https://stackoverflow.com/questions/52742899/aligning-class-members-in-c11
// PureRT::assert_false_sharing_range_aligned(
//         reinterpret_cast<void*>(&readIndex_));
// PureRT::assert_false_sharing_range_aligned(
//         reinterpret_cast<void*>(&writeIndex_));
#endif
    }

    ~UntypedProducerConsumerQueue() {
        // We don't need to destruct anything that may still exist in our queue.
        // becasue we are just using direct POD types as the payload.
        std::free(records_);
        assert(sizeof(UntypedProducerConsumerQueue) % CACHE_LINE_BYTES == 0);
    }

    // writeAndCopyBuf does two things:
    // 1. inserts a new entry into the queue
    // 2. copies the payload from ptr_to_source_for_copy contents into the
    //    entry in the records_ structure.
    __attribute__((flatten)) inline bool
    writeAndCopyBuf(void const* const __restrict ptr_to_source_for_copy,
                    bool               assume_aligned_source_buf,
                    payload_count_type payload_count, int bytes_to_copy
#if COLLECT_THREAD_TIMELINE_DETAIL
                    ,
                    BenchmarkTimer& memcpy_timer
#endif
    ) {

        auto const currentWrite = writeIndex_.load(std::memory_order_relaxed);
        auto       nextRecord   = currentWrite + 1;
        if (nextRecord == size_) {
            nextRecord = 0;
        }

        // see https://getpocket.com/read/3571524993 for cached index
        // optimization
        if (nextRecord == readIndex_cached) {
            // here we update the cached record to really make sure that we
            // don't have room to do the write
            readIndex_cached = readIndex_.load(std::memory_order_acquire);
        }

        if (nextRecord != readIndex_cached) {
            sized_buf_ptr const sbp =
                    recordsAddressForSizedBufPtr(currentWrite);
            // write in the payload count at the beginning of the record
            static_cast<payload_count_type*>(sbp)[0] = payload_count;
            PureRT::assert_cacheline_aligned(sbp);

            void* const __restrict start_of_payload_buf =
                    static_cast<uint8_t*>(sbp) + buf_ptr_offset;

            assert(reinterpret_cast<uintptr_t>(sbp) %
                           sizeof(payload_count_type) ==
                   0);

            assert(buf_ptr_offset == sizeof(payload_count_type));
            // Useful for deep debugging with ASAN errors, etc.
            // fprintf(stderr,
            //         "MEMCPY [UPCQ: %p | records: %p | sbp: %p "
            //         "(records+%d)]: start_of_payload_buf: %p "
            //         " (%d "
            //         "bytes
            //         copied)\nrecords_bytes=%d\treadIdx=%d\twriteIdx=%"
            //         "d\t(write: "
            //         "records+%d)\tbytes_per_payload_buf=%d\tpadded_bytes_per_"
            //         "payload_buf=%d\n",
            //         this, records_, sbp,
            //         reinterpret_cast<uintptr_t>(sbp) -
            //                 reinterpret_cast<uintptr_t>(records_),
            //         start_of_payload_buf, bytes_to_copy,
            //         temp_remove_records_bytes, readIndex_.load(),
            //         writeIndex_.load(),
            //         (reinterpret_cast<uintptr_t>(start_of_payload_buf) -
            //          reinterpret_cast<uintptr_t>(records_)),
            //         bytes_per_payload_buf, padded_bytes_per_payload_buf);

            START_MEMCPY_TIMER;
            // TODO: compiler hint to say this is an aligned copy.
            // https://clang.llvm.org/docs/AttributeReference.html#align-value
            // possibly also use the aligned value of function return on
            // consider using faster memcpy library (benchmark it)
            // use restrict keyword on these datatypes and remove restrict
            // override defined in Makefile.misc
            // make the size a full cache line
            // and round UP the bytes to copy size to be full cache lines
            // and make the size of the allocation for each message cache
            // line aligned on both beginning and end of buffer
            if (assume_aligned_source_buf) {
                MEMCPY_IMPL(start_of_payload_buf,
                            __builtin_assume_aligned(ptr_to_source_for_copy,
                                                     CACHE_LINE_BYTES),
                            bytes_to_copy);
            } else {
                MEMCPY_IMPL(start_of_payload_buf, ptr_to_source_for_copy,
                            bytes_to_copy);
            }

            PAUSE_MEMCPY_TIMER;
            writeIndex_.store(nextRecord, std::memory_order_release);
            return true;
        }

        // queue is full
        return false;
    }

    // pointer to the value at the front of the queue (for use in-place) or
    // nullptr if empty.
    inline sized_buf_ptr frontPtr() {

        auto const currentRead = readIndex_.load(std::memory_order_relaxed);
        if (currentRead == writeIndex_cached) {
            // make sure writeIndex hasn't changed
            writeIndex_cached = writeIndex_.load(std::memory_order_acquire);
        }

        if (currentRead == writeIndex_cached) {
            // queue is empty
            return nullptr;
        }
        return recordsAddressForSizedBufPtr(currentRead);
    }

    // queue must not be empty
    void popFront() {
        auto const currentRead = readIndex_.load(std::memory_order_relaxed);
        assert(currentRead != writeIndex_.load(std::memory_order_acquire));

        auto nextRecord = currentRead + 1;
        if (nextRecord == size_) {
            nextRecord = 0;
        }

        // original version from folly: records_[currentRead].~T();
        // we don't need to call a destructor on our version; just keep
        // overwriting previous values.
        readIndex_.store(nextRecord, std::memory_order_release);
    }

    bool isEmpty() const {
        return readIndex_.load(std::memory_order_acquire) ==
               writeIndex_.load(std::memory_order_acquire);
    }

    bool isFull() const {
        auto nextRecord = writeIndex_.load(std::memory_order_acquire) + 1;
        if (nextRecord == size_) {
            nextRecord = 0;
        }
        if (nextRecord != readIndex_.load(std::memory_order_acquire)) {
            return false;
        }
        // queue is full
        return true;
    }

    // * If called by consumer, then true size may be more (because producer
    // may
    //   be adding items concurrently).
    // * If called by producer, then true size may be less (because consumer
    // may
    //   be removing items concurrently).
    // * It is undefined to call this from any other thread.
    size_t sizeGuess() const {
        int ret = writeIndex_.load(std::memory_order_acquire) -
                  readIndex_.load(std::memory_order_acquire);
        if (ret < 0) {
            ret += size_;
        }
        return ret;
    }

    // maximum number of items in the queue.
    size_t capacity() const { return size_ - 1; }

  private:
    // Private Members
    using AtomicIndex = std::atomic<unsigned int>;

    void*          records_;
    const uint32_t size_;
    const uint32_t bytes_per_payload_buf; // here an "entry" is only the
                                          // payload, not the payload_count
#if PCV3_OPTION_USE_PADDED_RECORDS
    // number of bytes for each entry plus padding to fill out the cache
    // line
    // TODO: change this name to include the fact that the size as well
    const uint32_t padded_bytes_per_payload_buf;
#endif

#if PCV3_OPTION_PREVENT_MEMBER_FALSE_SHARING
    // padding at beginning of object
    // char pad0_[CACHE_LINE_BYTES];
    // Align to 64 to prevent false sharing between readIndex_ and
    // writeIndex_
    alignas(CACHE_LINE_BYTES) AtomicIndex readIndex_{0};
    alignas(CACHE_LINE_BYTES) size_t writeIndex_cached{0};
    alignas(CACHE_LINE_BYTES) AtomicIndex writeIndex_;
    alignas(CACHE_LINE_BYTES) size_t readIndex_cached{0};

    // pad so writeIndex doesn't false share with records_
    char pad1_[CACHE_LINE_BYTES - 2 * sizeof(AtomicIndex) - 2 * sizeof(size_t)];
#else
#error don't ever enable this.
    AtomicIndex readIndex_;
    AtomicIndex writeIndex_;
#endif

    //// Private Methods
    // helper function to index into records_ using pointer arithmetic given
    // that are using a void* generic container.
    inline sized_buf_ptr recordsAddressForSizedBufPtr(const int index) const {
        const sized_buf_ptr a = static_cast<uint8_t*>(records_) +
                                (padded_bytes_per_payload_buf * index);
        return a;
    }

    // figures out the total bytes per entry, possibly with cache line
    // padding if appropriate preprocessor options are set.
    uint32_t
    calculateTotalBytesPerEntry(uint32_t bytes_per_payload_entry) const {
        const uint32_t total_entry_bytes =
                sizeof(payload_count_type) + bytes_per_payload_entry;
#if PCV3_OPTION_USE_PADDED_RECORDS
        const unsigned int mod = total_entry_bytes % CACHE_LINE_BYTES;
        const unsigned int cache_line_pad_bytes =
                (mod == 0) ? 0 : CACHE_LINE_BYTES - mod;
        return total_entry_bytes + cache_line_pad_bytes;
#else
        // no cache line padding
        return total_entry_bytes;
#endif
    } // calculateTotalBytesPerEntry
};
