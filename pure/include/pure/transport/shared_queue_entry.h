// Author: James Psota
// File:   shared_queue_entry.h 

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

#ifndef SHARED_QUEUE_ENTRY_H
#define SHARED_QUEUE_ENTRY_H

#include <stdint.h>

using payload_count_type = uint32_t;
using buffer_t           = void*;

struct shared_queue_entry_t {
#if PCV2_OPTION_USE_PADDED_BUF_PTR
    alignas(CACHE_LINE_BYTES)
#endif
            // TODO: work in progress
            payload_count_type payload_count;
    buffer_t buf;
#if PCV2_OPTION_USE_PADDED_BUF_PTR
    // only include cache line padding (to prevent false sharing)
    // if this option is enabled.
    using cache_pad_t = unsigned char;
    static_assert(sizeof(cache_pad_t) == 1,
                  "Expected this to be one byte in size.");
    static_assert(sizeof(payload_count_type) + sizeof(buffer_t) <=
                          CACHE_LINE_BYTES,
                  "Expected members to fit in cache line.");
    cache_pad_t __cache_line_padding[CACHE_LINE_BYTES -
                                     sizeof(payload_count_type) -
                                     sizeof(buffer_t)];
#endif
    shared_queue_entry_t(payload_count_type _payload_count, buffer_t _buf);

    // ~shared_queue_entry_t()                                 = default;
    // shared_queue_entry_t(const shared_queue_entry_t& other) = default;
    // shared_queue_entry_t(shared_queue_entry_t&& other) noexcept = default;
    // shared_queue_entry_t& operator=(const shared_queue_entry_t& other) = default;
    // shared_queue_entry_t& operator=(shared_queue_entry_t&& other) = default;
}; // ends struct shared_queue_entry_t

#endif